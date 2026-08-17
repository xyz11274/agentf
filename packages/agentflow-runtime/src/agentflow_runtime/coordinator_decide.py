#!/usr/bin/env python3
"""
Coordinator Decide — event-driven coordinator node.

Each time the framework triggers this node, it executes ONE LLM decision:
  - First trigger: dispatcher forwarded user request → plan & dispatch
  - Subsequent triggers: skill completed → evaluate result → dispatch more or respond

The "loop" is provided by the framework: skill completion → re-trigger coordinator.

Environment variables (injected by the worker):
  AF_INPUT               — The user's original request (first trigger) or coordinator message
  AF_DIALOGUE_ID         — Current dialogue ID
  AF_SESSION_ID          — Current session ID
  AF_DATABUS_UDS_FILE    — UDS socket for runtime DataBus access
  AF_COMPLETED_SKILL_NAME — Name of the skill that just completed (re-trigger)
  AF_SKILL_SUCCESS       — "true"/"false" whether the skill succeeded
"""

import copy
import json
import logging
import os
import sys
import time
from datetime import datetime
from typing import Any
import uuid

# ── Add nodes/ to sys.path for local imports (before .venv activation) ──
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from agentflow_runtime.databus_client import DataBusClient
from agentflow_runtime.compaction import (
    compress_messages,
    get_compaction_threshold,
    get_model_max_tokens,
)
from agentflow_runtime.llm_call import (
    call_llm,
    get_recorder,
    resolve_llm_config,
)
from agentflow_runtime.common.agent_config import AgentConfig
from agentflow_runtime.common.runtime_context import (
    completed_skill_name as _completed_skill_name,
    dialogue_id as _dialogue_id,
    input as _input,
    session_id as _session_id,
    skill_artifacts as _skill_artifacts,
    skill_result as _skill_result,
    skill_success as _skill_success,
)
from agentflow_runtime.observability import emit_event
from agentflow_runtime import events as hook_events
from agentflow_runtime import mounts as hook_mounts
from agentflow_runtime.context import AgentContext
from agentflow_runtime.hooks import load_hooks_from_dir, trigger as hook_trigger
from agentflow_runtime.trace import emit_span, SpanKind, SpanStatus
from agentflow_runtime.parse_af_input import parse_af_input, parse_stream
from prompts import (
    COORDINATOR_FALLBACK_SYSTEM,
    DECISION_FAILURE_PROMPT,
    DECISION_SUCCESS_PROMPT,
    PROTOCOL_TEMPLATE,
    RUN_SKILLS_TOOL_DEFINITION,
    SKILL_COMPLETION_TEMPLATE,
)
from agentflow_runtime.round_reminder import build_round_reminder
# 霰弹式收敛(2026-08-14):产物处理/parse_agent_md/e2e_response/llm_error 兜底等
# 与 v2(coordinator_agent_loop.py)共享的纯逻辑统一收口到 coordinator_common,
# 本模块不再各自维护重复实现(单一真相源 F4)。
from agentflow_runtime.coordinator_common import (
    append_artifacts_markdown,
    bind_package_paths,
    is_empty_llm_response,
    llm_error_response,
    parse_agent_md,
    put_e2e_response,
    save_session_messages,
)

NODES_DIR = os.path.dirname(os.path.abspath(__file__))
AGENT_PACKAGE_DIR = os.path.dirname(NODES_DIR)
AGENT_MD_PATH = os.path.join(AGENT_PACKAGE_DIR, "agent.md")

# These are computed at import time from the pip package location.
# When used via entry files, the entry file overrides them via _m.__dict__.
# Functions that need these should use the module-level values directly
# (they will see the overridden values from the entry file).


def _get_agent_package_dir() -> str:
    """Return the agent package directory, respecting entry file overrides."""
    return AGENT_PACKAGE_DIR


def _get_agent_md_path() -> str:
    """Return the agent.md path, respecting entry file overrides."""
    return AGENT_MD_PATH

# ── Load .env from package root (business env vars: API_KEY, AF_LLM_*) ──
# Framework has no mechanism for business env; dotenv fills the gap.
# 策略：python-dotenv 首选（官方库更完整），装不上时兜底走 stdlib 零依赖解析器
#（agentflow_runtime.common.env_loader）——避免沙箱 DNS 不可达装不上 dotenv 导致业务断链
_DOTENV_PATH = os.path.join(AGENT_PACKAGE_DIR, ".env")


def _load_dotenv() -> None:
    """Load .env from the agent package directory, if it exists.
    Uses AGENT_PACKAGE_DIR from globals() to respect entry file overrides.

    走 python-dotenv 优先；未装时兜底走 stdlib 解析器（agentflow_runtime.common.env_loader）。
    """
    # 测试隔离开关：AF_SKIP_DOTENV=1 时跳过 .env 加载（override=True 会覆盖
    # 测试传入的环境变量，如 AF_LLM_BASE_URL，导致单测意外打真实 API）。
    if os.environ.get("AF_SKIP_DOTENV", "").lower() in ("1", "true"):
        print("[flow] dotenv: skipped (AF_SKIP_DOTENV set)", file=sys.stderr)
        return
    p = os.path.join(AGENT_PACKAGE_DIR, ".env")
    print(f"[flow] dotenv: path={p} exists={os.path.isfile(p)}", file=sys.stderr)
    if os.path.isfile(p):
        try:
            from dotenv import load_dotenv
            load_dotenv(p, override=True)
            print("[flow] dotenv: loaded OK (python-dotenv)", file=sys.stderr)
        except ImportError:
            # dotenv 装不上（沙箱 DNS 不可达等场景）→ 兜底走 stdlib 解析器
            from agentflow_runtime.common.env_loader import load_env_file
            n = load_env_file(p, override=True)
            print(f"[flow] dotenv: loaded OK (stdlib fallback, {n} vars)", file=sys.stderr)
        except Exception as exc:
            print(f"[dotenv] ERROR: load failed: {exc}", file=sys.stderr)
            logging.error("dotenv.load failed: %s", exc)


# _load_dotenv() is called from execute() to respect entry file overrides


# ── Helper functions (must be defined before module-level code below) ──
# 霰弹式收敛(2026-08-14):RE_ENV_VAR/_expand_env/parse_agent_md 已上移
# coordinator_common(见顶部 import),此处不再重复定义。


# ---------------------------------------------------------------------------
# Framework protocol — auto-injected into coordinator system prompt
# ---------------------------------------------------------------------------

# Build available-skills list from agent.md (lazy: re-reads when entry file overrides AGENT_MD_PATH)
_AVAILABLE_SKILLS: list[str] = []
_frontmatter: dict[str, Any] = {}


def _init_skills() -> None:
    """Init skills from agent.md. Re-runs on each call to respect entry file overrides."""
    global _AVAILABLE_SKILLS, _frontmatter, _skills_csv, COORDINATOR_PROTOCOL, RUN_SKILLS_TOOL
    _AVAILABLE_SKILLS = []
    _frontmatter, _ = parse_agent_md(AGENT_MD_PATH)
    _raw_skills = _frontmatter.get("skills", [])
    if isinstance(_raw_skills, list):
        for _s in _raw_skills:
            _name = _s if isinstance(_s, str) else _s.get("name", "")
            if _name and _name not in _AVAILABLE_SKILLS:
                _AVAILABLE_SKILLS.append(_name)
    _skills_csv = ", ".join(_AVAILABLE_SKILLS)
    _now = datetime.now().strftime("%Y-%m-%d %H:%M:%S %Z")
    COORDINATOR_PROTOCOL = PROTOCOL_TEMPLATE.format(available_skills=_skills_csv, current_time=_now)
    global RUN_SKILLS_TOOL
    RUN_SKILLS_TOOL = copy.deepcopy(RUN_SKILLS_TOOL_DEFINITION)
    RUN_SKILLS_TOOL["function"]["parameters"]["properties"]["skills"]["items"]["properties"]["name"]["enum"] = list(_AVAILABLE_SKILLS)


_init_skills()  # Module-level init; re-called in execute() to respect entry file overrides


# ---------------------------------------------------------------------------
# run_skills interception → DataBus → framework next_skills pipeline
# ---------------------------------------------------------------------------

def _is_duplicate_dispatch(completed_skill: str, skill_names: list,
                           force_requested: bool,
                           last_prompt: str | None = None) -> bool:
    """F2 拦截判定:同一 dialogue 内重复 dispatch 刚完成的 skill(无 force)→ True。

    F1(提示词)无法完全阻止 LLM 偶发二次 dispatch,此处代码层强制拦截——
    避免重复执行整段 skill 浪费耗时。force=true 是显式逃生口,不拦截。

    判定维度(2026-08-13 升级):**skill 名 + 输入 prompt 双维度**。
    - 同名 skill 且本次 prompt 与上次 dispatch 该 skill 的 prompt 相同 → 重复,拦截。
    - 同名但 prompt 不同(用户补充信息/新任务指令 → 同 skill 重跑) → 不算重复,放行。
    - last_prompt 缺失(None)时保守按"同名即重复"处理,维持原语义。
    """
    if not completed_skill or force_requested:
        return False
    for _s in skill_names:
        name = _s.get("name") if isinstance(_s, dict) else str(_s)
        if name != completed_skill:
            continue
        prompt = _s.get("prompt", "") if isinstance(_s, dict) else ""
        if last_prompt is not None and prompt != last_prompt:
            # 同名但 prompt 不同 → 新任务,不算重复
            continue
        return True
    return False


def handle_run_skills_call(args: dict, databus: DataBusClient,
                           session_id: str, dialogue_id: str) -> str:
    """Intercept run_skills tool_call → write dispatch info to DataBus → trigger framework.

    All skills are routed through a single 'skill-runner' task.
    Real skill identity is carried by exec_skill_key → DataBus dispatch manifest.
    """
    skills = args.get("skills", [])
    reason = args.get("reason", "")

    # ① Write per-skill dispatch manifest to DataBus (must be BEFORE next_skills)
    #    Key: dispatch:{N} → value: {skill_name, prompt, load_skills}
    next_skills_specs = []
    dispatch_round = uuid.uuid4().hex[:12]
    for i, spec in enumerate(skills, 1):
        dispatch_key = f"dispatch:{dialogue_id}:{dispatch_round}:{i}"
        dispatch_data = {
            "skill_name": spec["name"],
            "prompt": spec.get("prompt", ""),
            "load_skills": spec.get("load_skills", []),
        }
        databus.put_global(dispatch_key, json.dumps(dispatch_data, ensure_ascii=False))

        # 记录该 skill 最近一次 dispatch 的 prompt(供 re-trigger 时 F2 双维度
        # 重复判定对比:同名 skill + 同 prompt 才算重复,新 prompt 放行)。
        databus.put_global(f"last_prompt:{dialogue_id}:{spec['name']}",
                           spec.get("prompt", ""))

        # ② All skills go through 'skill-runner', differentiated by exec_skill_key
        # force 是 coordinator 显式 opt-in 的"重排逃生口"（builtin_dispatcher.h:28
        # / prompts/coordinator.py:29），不是默认路径。硬编码 force=True 会导致
        # 同一 dialogue 内多轮 run_skills 触发 framework 误判 force re-launch
        #（实测 2026-08-11 死链：21:30:39 forge-image-gen 完成 → 21:31:00 第二轮
        # run_skills 调 outfit-stylist，C++ 看到 force=true 的 skill-runner 且
        # skill-runner 已在 launched_skills → 用同一 task_id 再 launch 一次 →
        # UI 收到 _final 后又来一轮 llm_chunk → 渲染崩）。
        next_skills_specs.append({
            "name": "skill-runner",
            "exec_skill_key": dispatch_key,
            "force": bool(spec.get("force", False)),
        })

    # ③ Write next_skills → framework takes over scheduling
    databus.put("next_skills", json.dumps(next_skills_specs, ensure_ascii=False))

    # ④ Return confirmation
    return json.dumps({
        "status": "dispatched",
        "skills": [s["name"] for s in skills],
        "message": f"已调度 {len(skills)} 个 skill，等待执行结果...",
        "reason": reason
    }, ensure_ascii=False)


def build_skill_completion_message(completed_skill: str, skill_success: str,
                                   completed_skill_result: str,
                                   collected_results: list) -> str:
    """Build the coordinator model input, preserving structured failure evidence."""
    if completed_skill_result:
        try:
            parsed_result = json.loads(completed_skill_result)
        except (json.JSONDecodeError, TypeError):
            parsed_result = completed_skill_result
        skill_results = [{"skill": completed_skill, "result": parsed_result}]
    else:
        skill_results = collected_results

    decision = (
        DECISION_FAILURE_PROMPT
        if str(skill_success).lower() != "true"
        else DECISION_SUCCESS_PROMPT
    )
    return SKILL_COMPLETION_TEMPLATE.format(
        completed_skill=completed_skill,
        skill_success=skill_success,
        skill_results=json.dumps(skill_results, ensure_ascii=False, indent=2),
        decision=decision,
    )


# ---------------------------------------------------------------------------
# Skill result collection
# ---------------------------------------------------------------------------

def collect_skill_results(databus: DataBusClient, skill_names: list) -> list:
    """Read completed skill results from DataBus.

    databus.get may return a dict (SDK auto-deserialized) or a str (raw JSON).
    Handle both to avoid TypeError on json.loads(dict).
    """
    results = []
    for name in skill_names:
        raw = databus.get(f"skill_result:{name}")
        if not raw:
            continue
        if isinstance(raw, dict):
            results.append({"skill": name, "result": raw})
        else:
            try:
                results.append({"skill": name, "result": json.loads(raw)})
            except (json.JSONDecodeError, TypeError):
                results.append({"skill": name, "result": str(raw)})
    return results


# 霰弹式收敛(2026-08-14):append_artifacts_markdown / llm_error_response /
# rewrite_local_artifact_links / rewrite_local_image_links / local_artifact_url /
# put_e2e_response 已上移 coordinator_common(见顶部 import),此处不再重复定义。


# ---------------------------------------------------------------------------
# Main — single LLM decision per trigger
# ---------------------------------------------------------------------------


def execute():
    _t0 = time.time()
    # 霰弹式收敛(2026-08-14):公共模块 coordinator_common 的产物 URL 生成
    # 依赖模块级 AGENT_PACKAGE_DIR——entry file 只注入本模块,必须同步到公共模块,
    # 否则 local_artifact_url 落到 pip 包路径(v2 对 skill_executor 同款同步模式)。
    bind_package_paths(AGENT_PACKAGE_DIR, AGENT_MD_PATH, NODES_DIR)
    _load_dotenv()      # Load .env (respects entry file overrides of AGENT_PACKAGE_DIR)
    _init_skills()      # Init skills from agent.md (respects entry file overrides)
    raw_input = _input()
    dialogue_id = _dialogue_id()
    session_id = _session_id()

    # Framework-injected skill completion info (from node inputs → AF_ env vars)
    completed_skill = _completed_skill_name()
    skill_success = _skill_success()
    completed_skill_result = _skill_result()
    completed_skill_artifacts = _skill_artifacts()

    max_iterations = AgentConfig.coordinator_max_iterations()
    max_completion_tokens = AgentConfig.max_completion_tokens()

    frontmatter, agent_md_body = parse_agent_md(AGENT_MD_PATH)
    model_cfg = frontmatter.get("model", {})
    # model can be a plain string (e.g. "MiniMax-M3") or a dict {name: "..."}
    agent_md_model = model_cfg if isinstance(model_cfg, str) else model_cfg.get("name", "")

    # 使用 fallback 链解析最终 LLM 配置
    llm_cfg = resolve_llm_config(agent_md_model, node_type="coordinator")
    model_name = llm_cfg["model"]
    api_key = llm_cfg["api_key"]
    base_url = llm_cfg["base_url"]

    # ── hooks: 加载 agent 包 harness/hooks（幂等，无 hooks 目录则零开销）──
    load_hooks_from_dir(os.path.join(AGENT_PACKAGE_DIR, "harness", "hooks"))
    # ── hooks: 回合级入口挂载点（观察；messages 尚未构造，ctx 空上下文）──
    hook_trigger(hook_mounts.AGENT_DIALOGUE_START, {
        "session_id": session_id,
        "dialogue_id": dialogue_id,
        "trigger_type": "skill_completion" if completed_skill else "first",
    }, ctx=AgentContext())

    # ── flow debug ──
    emit_event(hook_events.COORDINATOR_TRIGGER, {
        "trigger_type": "skill_completion" if completed_skill else "first",
        "completed_skill": completed_skill,
        "skill_success": skill_success,
        "model": model_name,
        "max_iterations": max_iterations,
    })
    _t1 = time.time()

    databus = DataBusClient()
    recorder = get_recorder()
    # Read stream flag from AF_INPUT (bridge sends {"stream": bool, "messages": [...]})
    stream = parse_stream(_input())
    # Persist to DataBus for downstream nodes (skill_executor, etc.)
    if stream:
        databus.put("stream", "true")

    # 读取刚完成的 skill 最近一次 dispatch 的 prompt(F2 双维度重复判定用:
    # 同名 skill + 同 prompt 才算重复;同名不同 prompt = 新任务放行)。
    last_prompt = None
    if completed_skill and databus.is_connected():
        try:
            last_prompt = databus.get_global(f"last_prompt:{dialogue_id}:{completed_skill}")
        except Exception as exc:
            print(f"[flow]   WARN: read last_prompt failed: {exc}", file=sys.stderr)
            last_prompt = None

    # ── Coordinator iteration tracking ──
    # First trigger (no completed_skill or dummy_pre_skill hook) starts a new
    # dialogue → reset counter to 1. Re-trigger (skill completed) → +1.
    # This prevents stale counter from a previous dialogue leaking into a new
    # one within the same session.
    is_first_trigger = not (completed_skill and completed_skill != "dummy_pre_skill")
    if is_first_trigger:
        current_round = 1
    else:
        raw_round = databus.get("_coordinator_round") if databus.is_connected() else None
        try:
            current_round = int(raw_round) + 1 if raw_round is not None else 1
        except (ValueError, TypeError):
            current_round = 1
    if databus.is_connected():
        databus.put("_coordinator_round", str(current_round))

    # ── Max iterations guard: break before LLM call ──
    if current_round > max_iterations:
        emit_event(hook_events.COORDINATOR_MAX_ITERATIONS, {"round": current_round, "max_iterations": max_iterations}, level="WARN")
        response_text = llm_error_response(completed_skill_artifacts)
        # ── hooks: 回合级出口（观察，不可干预——固定错误文案）──
        hook_trigger(hook_mounts.AGENT_DIALOGUE_END, {
            "session_id": session_id,
            "dialogue_id": dialogue_id,
            "status": "max_iterations",
            "response_text": response_text,
        }, ctx=AgentContext())
        output = {"response": response_text, "status": "max_iterations_reached"}
        print(json.dumps(output, ensure_ascii=False))
        put_e2e_response(databus, response_text)
        print(f"[timing] max_iterations total={time.time()-_t0:.1f}s", file=sys.stderr)
        return

    _t2 = time.time()
    print(f"[timing] init={_t2-_t1:.1f}s", file=sys.stderr)

    # Restore cross-trigger conversation history (llm namespace, session-persistent)
    history = recorder.read_session_messages() if recorder.is_connected else []

    # Assemble system prompt = framework protocol + business intent
    system_prompt = COORDINATOR_PROTOCOL + "\n\n---\n\n" + (agent_md_body or
                     COORDINATOR_FALLBACK_SYSTEM)

    messages = [{"role": "system", "content": system_prompt}]

    # Parse AF_INPUT to get the conversation messages from bridge
    # AF_INPUT format: {"stream": bool, "messages": [{role, content}, ...]}
    # Falls back to plain text via parse_af_input() for backward compatibility.
    af_messages: list[dict] = []
    try:
        parsed = json.loads(raw_input)
        if isinstance(parsed, dict) and "messages" in parsed and isinstance(parsed["messages"], list):
            af_messages = parsed["messages"]
    except (json.JSONDecodeError, TypeError):
        pass

    # If the inline JSON parsing failed (plain text input), use parse_af_input
    # which handles both plain text and JSON format, then convert to messages.
    if not af_messages:
        content_parts = parse_af_input(raw_input)
        if content_parts:
            # Convert parse_af_input format [{type, text}, ...] to
            # LLM messages format [{role, content}, ...]
            for part in content_parts:
                if isinstance(part, dict) and part.get("type") == "text" and part.get("text"):
                    af_messages.append({"role": "user", "content": part["text"]})
                elif isinstance(part, dict) and part.get("role") and part.get("content"):
                    # Already in messages format (from Bridge JSON)
                    af_messages.append(part)

    # Construct this trigger's input
    # dummy_pre_skill is a passthrough hook — treat as first trigger (no result to evaluate)
    if completed_skill and completed_skill != "dummy_pre_skill":
        # Re-trigger: use persisted history + skill completion message
        messages.extend(history)
        # collect_skill_results is a 30s databus.get() fallback — skip when env var has the data.
        collected = [] if completed_skill_result else collect_skill_results(databus, [completed_skill])
        messages.append({"role": "user", "content": build_skill_completion_message(
            completed_skill,
            skill_success,
            completed_skill_result,
            collected,
        )})
    else:
        # First trigger: use the messages array from AF_INPUT directly
        # (bridge has already assembled history + this turn's user input)
        messages.extend(af_messages)

    # Single LLM decision (tools: [run_skills])
    # 4000 tokens to avoid finish=length truncation on long responses.
    #
    # Transport-layer retry (SSL/DNS/timeout) lives INSIDE call_llm — see
    # llm_call.py:_is_transient_http_error. Coordinator does NOT duplicate
    # network retry here; it only handles _llm_error as a final fallback
    # (after call_llm has exhausted its own retries).

    # ── Token budget: compress messages if exceeding threshold ──
    # compress_messages 内部按设计文档 §6 格式打印 [flow] compaction: 日志行
    model_max_tokens = get_model_max_tokens()
    compaction_threshold = get_compaction_threshold()
    budget = int(model_max_tokens * compaction_threshold)
    compressed, level = compress_messages(
        messages, budget,
        session_id=_session_id(),
        dialogue_id=_dialogue_id(),
        fold_dir=AgentConfig.fold_dir(),
        protect_last_rounds=AgentConfig.protect_last_rounds(),
        hard_compression_enabled=AgentConfig.hard_compression_enabled(),
        hard_compression_model=AgentConfig.hard_compression_model(),
        node_type="coordinator",
    )
    if level != "skip":
        messages = compressed

    # ── Per-round budget reminder (delegated to shared module) ──
    reminder = build_round_reminder(
        round_i=current_round - 1,  # 0-based for shared module
        max_rounds=max_iterations,
        elapsed=time.time() - _t0,
        node_timeout_sec=AgentConfig.node_timeout_sec(),
        custom_hint="本轮必须输出，可以是任务结论或当前执行结果",
    )
    if reminder:
        messages.append(reminder)

    # ── Coordinator 工具循环 ──────────────────────────────────────────
    # run_skills 被拦截(重复 dispatch)/参数无效时,把工具调用结果作为
    # tool result 返回给模型(与 skill_executor 同模式),让模型重新决策;
    # 直到模型直接输出 final response / 成功 dispatch skill / 达到轮次上限。
    _tool_loop_round = 0
    while True:
        _tool_loop_round += 1
        # ── 工具循环轮次上限保护 ──
        if _tool_loop_round > max_iterations:
            emit_event(hook_events.COORDINATOR_TOOL_LOOP_MAX_ROUNDS, {
                "round": current_round,
                "tool_loop_round": _tool_loop_round,
                "max_iterations": max_iterations,
            }, level="WARN")
            response_text = "已达协调工具循环上限，基于已有结果输出。"
            output = {"response": response_text, "status": "tool_loop_max_rounds"}
            print(json.dumps(output, ensure_ascii=False))
            put_e2e_response(databus, response_text)
            print(f"[timing] tool_loop_max_rounds total={time.time()-_t0:.1f}s", file=sys.stderr)
            return

        _t4 = time.time()
        print(f"[timing] llm_call_start={_t4-_t0:.1f}s", file=sys.stderr)
        # ── Progress: coordinator is calling LLM (may take >60s) ──
        try:
            if 'databus' in dir():
                databus.response_push("coordinator 决策中…\n", type="progress",
                                      skill="coordinator", status="running",
                                      source="coordinator")
        except Exception as exc:
            print(f"[flow]   WARN: coordinator progress push failed: {exc}", file=sys.stderr)
        # ── Interrupt flag poll (design §3.3.2) ──────────────────────────
        # daemon 的 handle_dialogue_interrupt 在 cleanup 前写入 global sys flag
        # "/session/{sid}/sys/interrupt:{did}"。每轮 call_llm 之前轮询，命中则
        # 写 interrupted 最终响应给 Bridge 走正常断流，自己 return。
        # 轮询失败（DataBus 抖动）不阻断主流程（I-INT-5）。
        if databus.is_connected():
            try:
                _flag = databus.get_global(f"interrupt:{dialogue_id}")
                if _flag == "true":
                    emit_event(hook_events.COORDINATOR_INTERRUPTED, {
                        "round": current_round,
                        "elapsed_s": round(time.time() - _t0, 1),
                    }, level="WARN")
                    response_text = "对话已被取消"
                    # ── hooks: 回合级出口（观察，不可干预——固定中断文案）──
                    hook_trigger(hook_mounts.AGENT_DIALOGUE_END, {
                        "session_id": session_id,
                        "dialogue_id": dialogue_id,
                        "status": "interrupted",
                        "response_text": response_text,
                    }, ctx=AgentContext(messages=messages))
                    output = {"response": response_text, "status": "interrupted"}
                    # Bridge 从 stdout JSON 读 coordinator 响应（与正常完成路径同款，
                    # 见行 551-552 print(json.dumps(output))）。中断必须走同一通道，
                    # 否则 Bridge 收不到响应会 hang 到超时。
                    print(json.dumps(output, ensure_ascii=False))
                    if databus.is_connected():
                        try:
                            # 清掉可能的旧 response key，避免下轮误读（与正常完成路径行 882 对齐）
                            databus.delete("response")
                        except Exception as exc:
                            print(f"[flow]   WARN: interrupt response delete failed: {exc}", file=sys.stderr)
                    print(f"[timing] interrupted total={time.time()-_t0:.1f}s", file=sys.stderr)
                    return
            except Exception as exc:
                print(f"[flow]   WARN: interrupt flag poll failed: {exc}", file=sys.stderr)
        with emit_span(
            SpanKind.LLM, hook_events.SPAN_COORDINATOR_LLM_ROUND,
            tags={
                "round": str(current_round),
                "messages": str(len(messages)),
                "model": model_name,
                "stream": str(stream),
                "trigger_location": "python_direct",
            },
        ) as _llm_span:
            _llm_span.set_tag("dialogue_id", _dialogue_id() or "")
            # ── hooks: LLM 调用前挂载点（注入 messages / 阻断本轮，v1 决策循环）──
            _llm_ctx = AgentContext(messages=messages)
            hook_trigger(hook_mounts.COORDINATOR_V1_BEFORE_LLM_CALL, {
                "round": current_round,
                "model": model_name,
                "messages_count": len(messages),
            }, can_intervene=True, ctx=_llm_ctx)
            if _llm_ctx.blocked:
                # 阻断本轮 LLM 调用：直接走最终响应出口（无 tool_call 语义）
                raw = {"choices": [{"message": {"role": "assistant", "content": "（LLM 调用已被 hook 阻断）"}}]}
                _llm_blocked = True
            else:
                raw = call_llm(
                    messages, tools=[RUN_SKILLS_TOOL],
                    model=model_name,
                    base_url=base_url,
                    api_key=api_key,
                    max_completion_tokens=max_completion_tokens,
                    stream=stream,
                    source="coordinator",
                    extra_body={
                        "temperature": AgentConfig.llm_temperature(),
                        "top_p": AgentConfig.llm_top_p(),
                        "frequency_penalty": AgentConfig.llm_frequency_penalty(),
                        "presence_penalty": AgentConfig.llm_presence_penalty(),
                        "top_k": AgentConfig.llm_top_k() or None,
                        "stop": AgentConfig.llm_stop() or None,
                    }
                )
            _t5 = time.time()
            print(f"[timing] llm_call_end={_t5-_t0:.1f}s (llm={_t5-_t4:.1f}s)", file=sys.stderr)
            # ── hooks: LLM 调用后挂载点（检查/改写决策，v1）──
            hook_trigger(hook_mounts.COORDINATOR_V1_AFTER_LLM_CALL, {
                "round": current_round,
                "raw_choices": raw.get("choices", [{}])[0] if raw.get("choices") else {},
            }, ctx=_llm_ctx)

            # ── Observability: log raw LLM response structure ──
            _obs_choice = raw.get("choices", [{}])[0]
            _obs_msg = _obs_choice.get("message", {})
            _obs_tcs = _obs_msg.get("tool_calls")
            _obs_finish = _obs_choice.get("finish_reason", "?")
            _obs_content = _obs_msg.get("content", "")
            _obs_content_preview = _obs_content[:150].replace("\n", " ") if _obs_content else "(empty)"
            _obs_tc_count = len(_obs_tcs) if _obs_tcs else 0
            _obs_tc_names = [tc.get("function", {}).get("name", "?")
                             for tc in (_obs_tcs or [])]
            _llm_span.add_event(hook_events.COORDINATOR_LLM_RESULT, {
                "round": str(current_round),
                "finish_reason": _obs_finish,
                "tool_call_count": str(_obs_tc_count),
                "tool_call_names": ",".join(_obs_tc_names),
                "content_preview": _obs_content_preview,
                "llm_duration_s": str(round(_t5 - _t4, 1)),
                "total_duration_s": str(round(_t5 - _t0, 1)),
            })
            emit_event(hook_events.COORDINATOR_LLM_RESULT, {
                "round": current_round,
                "finish_reason": _obs_finish,
                "tool_call_count": _obs_tc_count,
                "tool_call_names": _obs_tc_names,
                "content_preview": _obs_content_preview,
                "llm_duration_s": round(_t5 - _t4, 1),
                "total_duration_s": round(_t5 - _t0, 1),
            }, duration_ms=(_t5 - _t4) * 1000)

            if raw.get("_llm_error"):
                err_msg = raw.get("error", "LLM call failed")
                _llm_span.set_status(SpanStatus.ERROR, err_msg)
                _llm_span.add_event(hook_events.COORDINATOR_LLM_ERROR, {
                    "round": str(current_round),
                    "error": err_msg,
                    "duration_s": str(round(_t5 - _t0, 1)),
                })
                emit_event(hook_events.COORDINATOR_LLM_ERROR, {
                    "round": current_round,
                    "error": err_msg,
                    "duration_s": round(_t5 - _t0, 1),
                }, level="ERROR")
                # Return fallback response instead of crashing — user gets something,
                # workflow completes, coordinator can be re-triggered.
                response_text = llm_error_response(completed_skill_artifacts)
                output = {"response": response_text}
                put_e2e_response(databus, response_text)
                _t6 = time.time()
                print(f"[timing] output={_t6-_t0:.1f}s total", file=sys.stderr)
                print(json.dumps(output, ensure_ascii=False))
                return

        # ── 检测 LLM 返回空内容（API 成功但未实际处理请求） ──
        # 典型信号：content="" + usage 全零 + finish_reason=stop
        # 这通常意味着 API key 无效、模型不支持流式、或参数配置错误。
        _raw_usage = raw.get("usage", {}) or {}
        _raw_choice = raw.get("choices", [{}])[0]
        _raw_msg = _raw_choice.get("message", {})
        _raw_content = _raw_msg.get("content", "")
        _raw_finish = _raw_choice.get("finish_reason", "")
        _raw_tc = _raw_msg.get("tool_calls")
        # 复用 coordinator_common.is_empty_llm_response(单一真相源 F4,霰弹式收敛)
        _is_empty_response = is_empty_llm_response(raw)
        if _is_empty_response:
            err_msg = "LLM returned empty response (zero tokens, no content, finish=stop)"
            emit_event(hook_events.COORDINATOR_LLM_ERROR, {
                "round": current_round,
                "error": err_msg,
                "duration_s": round(_t5 - _t0, 1),
            }, level="ERROR")
            response_text = llm_error_response(completed_skill_artifacts)
            output = {"response": response_text}
            put_e2e_response(databus, response_text)
            _t6 = time.time()
            print(f"[timing] output={_t6-_t0:.1f}s total", file=sys.stderr)
            print(json.dumps(output, ensure_ascii=False))
            return

        choice = raw.get("choices", [{}])[0]
        msg = choice.get("message", {})
        tool_calls = msg.get("tool_calls")
        messages.append(msg)

        if not tool_calls:
            # LLM made no tool_call → final response
            raw_response = msg.get("content", "")
            # Strip <think> tags: keep reasoning in trace, only show user-facing content
            import re as _re
            think_match = _re.search(r"</think>\s*", raw_response)
            if think_match:
                response_text = raw_response[think_match.end():].strip()
            else:
                response_text = raw_response
            response_text = append_artifacts_markdown(
                response_text, completed_skill_artifacts
            )
            emit_event(hook_events.COORDINATOR_FINAL_RESPONSE, {
                "round": current_round,
                "has_tool_call": False,
                "response_length": len(response_text),
                "elapsed_s": round(time.time() - _t0, 1),
            })
            # ── hooks: 最终响应前挂载点（改写 response_text；ctx 供注入/修改）──
            _final_ctx = AgentContext(messages=messages)
            _hook_payload = hook_trigger(
                hook_mounts.COORDINATOR_BEFORE_FINAL_RESPONSE,
                {"round": current_round, "response_text": response_text, "has_tool_call": False},
                can_intervene=True,
                ctx=_final_ctx,
            )
            response_text = _hook_payload.get("response_text", response_text)
            hook_trigger(hook_mounts.AGENT_DIALOGUE_END, {
                "session_id": session_id,
                "dialogue_id": dialogue_id,
                "status": "normal",
                "response_text": response_text,
                "elapsed_s": round(time.time() - _t0, 1),
            }, can_intervene=True, ctx=_final_ctx)
            print(json.dumps({"response": response_text}, ensure_ascii=False))
            _t6 = time.time()
            print(f"[timing] final_output={_t6-_t0:.1f}s total", file=sys.stderr)
            # ── hooks: 最终响应后挂载点（记录/审计）──
            hook_trigger(hook_mounts.COORDINATOR_AFTER_FINAL_RESPONSE, {
                "round": current_round,
                "response_length": len(response_text),
                "elapsed_s": round(time.time() - _t0, 1),
            }, ctx=_final_ctx)

            # Write to persistent DataBus key for e2e tests (bypasses RESPONSE_POLLER
            # which immediately removes the "response" key). Raw client writes to
            # global namespace, not dialogue-scoped, so any session can read it.
            if databus.is_connected():
                put_e2e_response(databus, response_text, log_timing=True)

            save_session_messages(recorder, messages)

            _t9 = time.time()
            print(f"[timing] script_exit={_t9-_t0:.1f}s", file=sys.stderr)
            return

        # ── Max iterations guard: if this is the last allowed round, force final ──
        #     output instead of dispatching more skills.
        if current_round >= max_iterations:
            raw_response = msg.get("content", "")
            import re as _re
            think_match = _re.search(r"</think>\s*", raw_response)
            if think_match:
                response_text = raw_response[think_match.end():].strip()
            else:
                response_text = raw_response or "已达协调轮次上限，基于已有结果输出。"
            response_text = append_artifacts_markdown(response_text, completed_skill_artifacts)
            emit_event(hook_events.COORDINATOR_MAX_ROUNDS_FORCED_OUTPUT, {
                "round": current_round,
                "max_iterations": max_iterations,
                "response_length": len(response_text),
            }, level="WARN")
            print(json.dumps({"response": response_text}, ensure_ascii=False))
            put_e2e_response(databus, response_text)
            save_session_messages(recorder, messages)
            print(f"[timing] max_rounds_forced_output total={time.time()-_t0:.1f}s", file=sys.stderr)
            return

        # Collect dispatched skill names for progress reporting (ordered set via dict)
        _dispatched_skills: dict[str, None] = {}
        for tc in tool_calls:
            func = tc.get("function", {})
            name = func.get("name", "")
            try:
                args = json.loads(func.get("arguments", "{}"))
            except json.JSONDecodeError:
                args = {}

            if name == "run_skills":
                skill_names = args.get("skills", [])
                # F2 拦截(CHANGE-20260811-055):连续 round 重复 dispatch 已完成的 skill(无 force)→ 拒绝。
                # F1(提示词)无法完全阻止 LLM 偶发二次 dispatch,此处代码层强制拦截——
                # 避免重复执行整段 skill 浪费耗时(超时现场 140842 同源:二次 dispatch 空 skills/重复执行)。
                force_requested = bool(args.get("force")) or any(
                    isinstance(_s, dict) and _s.get("force") for _s in skill_names
                )
                if _is_duplicate_dispatch(completed_skill, skill_names, force_requested,
                                          last_prompt):
                    result_str = json.dumps({
                        "error": f"skill '{completed_skill}' 刚完成，不得重复 dispatch（除非 force=true）",
                        "response": f"skill '{completed_skill}' 刚完成，基于其结果直接回复，不要重复执行。",
                    }, ensure_ascii=False)
                    emit_event(hook_events.COORDINATOR_DUPLICATE_SKILL_DISPATCH_BLOCKED, {
                        "round": current_round,
                        "skill": completed_skill,
                    }, level="WARN")
                else:
                    # Collect skill names for progress reporting
                    for _s in skill_names:
                        _sn = _s.get("name", "") if isinstance(_s, dict) else str(_s)
                        if _sn:
                            _dispatched_skills[_sn] = None
                    # ── hooks: skill 分发前挂载点（阻断本轮 dispatch，ctx.blocked 语义）──
                    _dispatch_ctx = AgentContext(messages=messages)
                    hook_trigger(
                        hook_mounts.COORDINATOR_BEFORE_SKILL_DISPATCH,
                        {
                            "round": current_round,
                            "skill_names": list(_dispatched_skills),
                        },
                        can_intervene=True,
                        ctx=_dispatch_ctx,
                    )
                    if _dispatch_ctx.blocked:
                        result_str = json.dumps({
                            "skipped_by_hook": True,
                            "response": "本轮 skill 分发已被 hook 阻断，请直接基于已有信息回复用户。",
                        }, ensure_ascii=False)
                    else:
                        result_str = handle_run_skills_call(args, databus, session_id, dialogue_id)
                        # ── hooks: skill 分发后挂载点（检查分发结果）──
                        hook_trigger(hook_mounts.COORDINATOR_AFTER_SKILL_DISPATCH, {
                            "round": current_round,
                            "skill_names": list(_dispatched_skills),
                            "result_length": len(result_str),
                        }, ctx=_dispatch_ctx)
            else:
                result_str = json.dumps({"error": f"Unknown tool: {name}"})

            messages.append({
                "role": "tool",
                "tool_call_id": tc.get("id", name),
                "content": result_str,
            })

        # Persist conversation history (coordinator yields control, waits for re-trigger)
        save_session_messages(recorder, messages)

        # Output coordinator_status + progress response for frontend
        _t6 = time.time()
        print(f"[timing] skill_dispatched={_t6-_t0:.1f}s", file=sys.stderr)
        if _dispatched_skills:
            skill_labels = "、".join(_dispatched_skills)
            with emit_span(
                SpanKind.SKILL, hook_events.SPAN_COORDINATOR_SKILL_DISPATCH,
                tags={
                    "round": str(current_round),
                    "skill_names": skill_labels,
                    "elapsed_s": str(round(_t6 - _t0, 1)),
                },
            ) as _dispatch_span:
                _dispatch_span.set_tag("dialogue_id", _dialogue_id() or "")
                _dispatch_span.add_event(hook_events.COORDINATOR_SKILLS_DISPATCHED, {
                    "round": str(current_round),
                    "skill_names": skill_labels,
                    "elapsed_s": str(round(_t6 - _t0, 1)),
                })
                emit_event(hook_events.COORDINATOR_SKILLS_DISPATCHED, {
                    "round": current_round,
                    "skill_names": skill_labels,
                    "elapsed_s": round(_t6 - _t0, 1),
                }, duration_ms=(_t6 - _t0) * 1000)

                # Push per-skill progress to databus (frontend progress area)
                if msg.get("content", "").strip():
                    try:
                        if 'databus' in dir() and databus.is_connected():
                            for skill_name_ in skill_labels.strip().split():
                                sn = skill_name_.strip()
                                if sn:
                                    databus.response_push(f"正在执行 {sn}\n", type="progress", skill=sn, status="running",
                                                           source="coordinator")
                    except Exception as exc:
                        print(f"[flow]   WARN: dispatch progress push failed: {exc}", file=sys.stderr)
                        _dispatch_span.add_event(hook_events.COORDINATOR_DISPATCH_PROGRESS_PUSH_FAILED, {
                            "error": str(exc)[:200],
                        })
                # Clean up "response" key to prevent stale progress message
                # from leaking into the next coordinator round.
                if databus.is_connected():
                    databus.delete("response")
                _dispatch_span.add_event(hook_events.COORDINATOR_DISPATCH_LOOP_END, {
                    "round": str(current_round),
                    "skills_dispatched": skill_labels,
                    "elapsed_s": str(round(_t6 - _t0, 1)),
                })
                emit_event(hook_events.COORDINATOR_DISPATCH_LOOP_END, {
                    "round": current_round,
                    "skills_dispatched": skill_labels,
                    "elapsed_s": round(_t6 - _t0, 1),
                })
            # 有 skill 成功 dispatch → yield 控制权:历史已在上方保存,
            # 输出 progress 后 return,等 skill 完成后 framework 重新触发 coordinator。
            return
        else:
            # 全部 tool_calls 被拦截(重复 dispatch)/参数无效/未知工具 → 没有 skill 成功 dispatch。
            # tool result 已在上面的 for 循环中回灌 messages——与 skill_executor 同模式:
            # 把工具调用结果(错误说明)返回给模型,continue 工具循环让模型重新决策
            # (直接输出 final response,或调度其他 skill),直到收敛或达到轮次上限。
            emit_event(hook_events.COORDINATOR_TOOL_LOOP_RETRY, {
                "round": current_round,
                "tool_loop_round": _tool_loop_round,
                "tool_calls": tool_calls,
                "elapsed_s": round(time.time() - _t0, 1),
            }, level="WARN")
            # 清理残留 next_skills(CHANGE-20260811-056):未 dispatch 时若不清理,
            # framework 在 WORKFLOW_COMPLETE 后仍 GET_FRAMEWORK_DATA 读到残留
            # next_skills → 误判有 pending skill → SSE 不 [DONE]。
            if databus.is_connected():
                try:
                    databus.delete("next_skills")
                except Exception as exc:
                    print(f"[flow] WARN: delete next_skills failed: {exc}", file=sys.stderr)
            print(f"[timing] tool_loop_retry={time.time()-_t0:.1f}s", file=sys.stderr)
            continue


if __name__ == "__main__":
    execute()
