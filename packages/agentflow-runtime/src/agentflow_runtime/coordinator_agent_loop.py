"""Coordinator v2 — Agent Loop 形态.

与 v1（coordinator_decide.py）的差异见：
    docs/design/features/agent-runtime/coordinator-as-agent-loop.md

核心理念：不暴露 run_skills 工具给 LLM，而是暴露 harness 工具集
（bash/read_file/write_file/analyze_image 等）。coordinator 退化为
一个普通的 agent loop 节点——和 skill_executor 内部的 tool 循环同款形态，
只是工具集和 system prompt 不同。

进程内 messages 天然保留上下文 → 避免跨 skill 重复分析（v1 的痛点）。

A/B 切换：通过 AF_COORDINATOR_VERSION 环境变量选 v1/v2，见
examples/agent/<pkg>/nodes/coordinator_decide.sh。
"""

from __future__ import annotations

import json
import logging
import os
import re
import sys
import time

from agentflow_runtime.databus_client import DataBusClient
from agentflow_runtime.compaction import (
    compress_messages,
    get_compaction_threshold,
    get_model_max_tokens,
)
from agentflow_runtime.llm_call import call_llm, get_recorder, resolve_llm_config
from agentflow_runtime.common.agent_config import AgentConfig
from agentflow_runtime.common.runtime_context import (
    dialogue_id as _dialogue_id,
    input as _input,
    session_id as _session_id,
)
from agentflow_runtime.observability import emit_event
from agentflow_runtime import events as hook_events
from agentflow_runtime import mounts as hook_mounts
from agentflow_runtime.context import AgentContext
from agentflow_runtime.hooks import load_hooks_from_dir, trigger as hook_trigger
from agentflow_runtime.trace import emit_span, SpanKind, SpanStatus
from agentflow_runtime.security import install_stderr_redaction
from agentflow_runtime.parse_af_input import parse_af_input, parse_stream
from agentflow_runtime.round_reminder import build_round_reminder
from agentflow_runtime.tool_policies import execute_in_batches

# v2 直接复用 skill_executor 的工具加载/沙箱调度，不重复实现
from agentflow_runtime.skill_executor import (
    _LocalToolRegistry,
    _inject_venv_env,
    _load_dotenv,
    load_skills_from_list,
    extract_artifact_paths,
    _ensure_artifacts_in_package,
    _detect_tool_call_loop,
    validate_tool_calls,
)

# prompts 包由 agent 包 nodes/../ 注入到 sys.path（与 v1 / skill_executor 同款机制）
from prompts import (
    COORDINATOR_FALLBACK_SYSTEM,
    SKILL_SECTION_TEMPLATE,
)
# 霰弹式收敛(2026-08-14):parse_agent_md / put_e2e_response / llm_error_response /
# 产物 URL 重写(append_artifacts_markdown) / session 保存与 v1 共享,
# 统一收口到 coordinator_common(单一真相源 F4),本模块不再各自维护重复实现。
from agentflow_runtime.coordinator_common import (
    append_artifacts_markdown,
    bind_package_paths,
    is_empty_llm_response,
    llm_error_response,
    parse_agent_md,
    put_e2e_response,
    save_session_messages,
)

# ── Module-level constants injected by entry file ──
# entry file（nodes/coordinator_decide_v2.py）会注入：
#   AGENT_PACKAGE_DIR, AGENT_MD_PATH, NODES_DIR
# 未注入时 fallback 到环境变量（直接 import 跑也兼容）
AGENT_PACKAGE_DIR: str = os.environ.get("AF_AGENT_PACKAGE_DIR", os.getcwd())
AGENT_MD_PATH: str = os.path.join(AGENT_PACKAGE_DIR, "agent.md")
# NODES_DIR fallback 与 AGENT_PACKAGE_DIR/AGENT_MD_PATH 同级:
# __main__/直接 import 未走 entry file 注入时 bind_package_paths 会读它,
# 缺失则 NameError(实测回归:bind_package_paths 引入 NODES_DIR 引用)。
NODES_DIR: str = os.path.dirname(os.path.abspath(__file__))


# ── v2 协议提示词：替代 v1 的 RUN_SKILLS_TOOL，告诉 LLM 直接调 harness 工具 ──
COORDINATOR_AGENT_LOOP_PROTOCOL = """\
你是 coordinator，一个自驱动的 agent loop 节点。

# 你的职责
根据用户请求，**直接调用 harness 工具**完成业务任务，直到能输出最终响应。
不要派发"skill"——skill 不存在了，你直接调工具执行。

# 工具使用
- 工具集是你能看到的 harness 工具（bash、read_file、write_file、analyze_image 等）
- 看到工具结果后继续决策，直到任务完成
- 工具结果会留在你的上下文里——**同一张图分析过一遍就够了**，下次直接用上下文里的结论

# 收敛条件
- 任务完成：直接输出最终响应（不调工具）
- 无法完成：基于已有信息输出兜底响应，告诉用户卡在哪里
- 达到轮次上限：基于已有结果输出

# 上下文保留
你的 messages 上下文天然保留所有工具调用与结果。跨轮调用工具时，**先看上下文里有没有已经做过的同类分析**，避免重复执行。
"""


# ── 辅助函数（与 v1 同款，但独立维护避免循环依赖） ──

def _get_agent_package_dir() -> str:
    return AGENT_PACKAGE_DIR


def _get_agent_md_path() -> str:
    return AGENT_MD_PATH


# parse_agent_md / put_e2e_response / llm_error_response 已上移 coordinator_common
# (见顶部 import)——霰弹式收敛 2026-08-14,不再各自维护重复实现。


# ── 主入口 ──

def execute() -> None:
    """v2 主入口：agent loop 形态的 coordinator.

    与 v1 的核心差异：
    1. tools 从 [RUN_SKILLS_TOOL] 换成 _LocalToolRegistry().get_tools()（harness 全工具）
    2. 工具调用后不 return（yield 给框架），而是 continue（自己 loop 到收敛）
    3. 没有 completed_skill / skill_success 等框架注入变量——coordinator 自己调工具
    4. 进程内 messages 天然保留上下文，避免跨 skill 重复分析
    """
    install_stderr_redaction()
    _t0 = time.time()
    # 霰弹式收敛(2026-08-14):公共模块 coordinator_common 的产物 URL 生成
    # 依赖模块级 AGENT_PACKAGE_DIR——entry file 只注入本模块,必须同步到公共模块,
    # 否则 local_artifact_url 落到 pip 包路径(v1 / skill_executor 同款同步模式)。
    bind_package_paths(AGENT_PACKAGE_DIR, AGENT_MD_PATH, NODES_DIR)
    _load_dotenv()       # 与 v1 / skill_executor 同款：加载 agent 包 .env
    _inject_venv_env()   # venv sentinel 注入（复用 skill_executor）

    raw_input = _input()
    dialogue_id = _dialogue_id()
    session_id = _session_id()

    max_rounds = AgentConfig.coordinator_max_iterations()
    max_completion_tokens = AgentConfig.max_completion_tokens()

    # 解析 agent.md
    frontmatter, agent_md_body = parse_agent_md(AGENT_MD_PATH)
    model_cfg = frontmatter.get("model", {})
    agent_md_model = model_cfg if isinstance(model_cfg, str) else model_cfg.get("name", "")

    # LLM 配置 fallback 链
    llm_cfg = resolve_llm_config(agent_md_model, node_type="coordinator")
    model_name = llm_cfg["model"]
    api_key = llm_cfg["api_key"]
    base_url = llm_cfg["base_url"]

    # ── 同步 skill_executor 模块的包路径 ──
    # v2 复用了 skill_executor 的 load_skills_from_list/_LocalToolRegistry，
    # 但 entry file 只注入了本模块（coordinator_agent_loop）的 AGENT_PACKAGE_DIR；
    # skill_executor 模块自身的 AGENT_PACKAGE_DIR 默认指向 pip 包目录
    # （src/harness/skills 不存在），必须同步为 agent 包路径，
    # 否则 SKILL.md / harness/tools 都找不到（实测返回 "[Skill 'x' 文档未找到]"）。
    import agentflow_runtime.skill_executor as _skill_executor_mod
    _skill_executor_mod.AGENT_PACKAGE_DIR = AGENT_PACKAGE_DIR
    _skill_executor_mod.AGENT_MD_PATH = AGENT_MD_PATH

    # hooks 加载（幂等，无 hooks 目录则零开销）
    load_hooks_from_dir(os.path.join(AGENT_PACKAGE_DIR, "harness", "hooks"))
    hook_trigger(hook_events.AGENT_DIALOGUE_START, {
        "session_id": session_id,
        "dialogue_id": dialogue_id,
        "trigger_type": "agent_loop",   # v2 没有 "first"/"skill_completion" 区分
        "coordinator_version": "v2",
    })

    # observability: 入口事件
    emit_event(hook_events.COORDINATOR_TRIGGER, {
        "trigger_type": "agent_loop",
        "coordinator_version": "v2",
        "model": model_name,
        "max_rounds": max_rounds,
    })
    _t1 = time.time()
    print(f"[timing] init={_t1-_t0:.1f}s", file=sys.stderr)

    databus = DataBusClient()
    recorder = get_recorder()

    # stream flag（与 v1 同款：从 AF_INPUT 解析后落 DataBus 给下游）
    stream = parse_stream(_input())
    if stream and databus.is_connected():
        databus.put("stream", "true")

    # ── 构造 system prompt = v2 协议 + SKILL.md 知识 + agent.md 业务意图 ──
    # skill scripts（outfit_vision.py 等）是 CLI 脚本，靠 SKILL.md 指引 + bash 调用——
    # 必须把 SKILL.md 内容注入 system prompt，coordinator 才知道怎么用 bash 调它们。
    skill_names: list[str] = []
    _raw_skills = frontmatter.get("skills", [])
    if isinstance(_raw_skills, list):
        for _s in _raw_skills:
            _nm = _s if isinstance(_s, str) else _s.get("name", "")
            if _nm and _nm not in skill_names:
                skill_names.append(_nm)
    skill_parts: list[str] = []
    if skill_names:
        skill_docs = load_skills_from_list(skill_names)
        for _nm, _content in skill_docs.items():
            skill_parts.append(SKILL_SECTION_TEMPLATE.format(name=_nm, content=_content))
    skills_block = "\n\n---\n\n".join(skill_parts) if skill_parts else ""
    system_prompt = COORDINATOR_AGENT_LOOP_PROTOCOL
    if skills_block:
        system_prompt += "\n\n---\n\n" + skills_block
    system_prompt += "\n\n---\n\n" + (agent_md_body or COORDINATOR_FALLBACK_SYSTEM)

    messages: list[dict] = [{"role": "system", "content": system_prompt}]

    # ── 载入历史 + 本次输入 ──
    # 跨 dialogue 靠 recorder（与 v1 同款）；dialogue 内靠进程内 messages（v2 核心收益）
    history = recorder.read_session_messages() if recorder.is_connected else []

    af_messages: list[dict] = []
    try:
        parsed = json.loads(raw_input)
        if isinstance(parsed, dict) and "messages" in parsed and isinstance(parsed["messages"], list):
            af_messages = parsed["messages"]
    except (json.JSONDecodeError, TypeError):
        pass

    if not af_messages:
        # plain text fallback
        content_parts = parse_af_input(raw_input)
        for part in content_parts:
            if isinstance(part, dict) and part.get("type") == "text" and part.get("text"):
                af_messages.append({"role": "user", "content": part["text"]})
            elif isinstance(part, dict) and part.get("role") and part.get("content"):
                af_messages.append(part)

    # 首次触发：用 history + 本次 af_messages
    # （v2 没有 re-trigger 概念，每次都是 agent loop 一次性跑完）
    messages.extend(history)
    messages.extend(af_messages)

    # ── 加载 harness 工具集（v2 核心：替代 v1 的 RUN_SKILLS_TOOL） ──
    # _LocalToolRegistry 复用 skill_executor 的实现：
    #   builtin + agent tools + configured skill tools + MCP
    tool_registry = _LocalToolRegistry(skill_names=skill_names)
    available_tools = tool_registry.get_tools()
    tool_names = [t["function"]["name"] for t in available_tools]

    emit_event(hook_events.COORDINATOR_SKILLS_DISPATCHED, {
        "round": 0,   # v2 启动轮——表示工具集就绪
        "skill_names": "(agent_loop)",
        "tools_available": tool_names,
        "model": model_name,
        "max_rounds": max_rounds,
    })
    print(f"[flow] v2 agent_loop tools={tool_names}", file=sys.stderr)

    # ── 节点级超时（v2 跑全程，要够长） ──
    node_timeout_sec = AgentConfig.node_execution_timeout()
    if not node_timeout_sec:
        node_timeout_sec = 600   # v2 fallback：全程跑，给 10 分钟
    _turn_start = time.time()

    # tool_call_history：每轮 tool_call 签名列表，检测死循环（复用 skill_executor）
    tool_call_history: list[list[tuple[str, str]]] = []
    generated_artifacts: list[str] = []

    # ── Agent loop（与 skill_executor 同款形态） ──
    for round_i in range(max_rounds):
        elapsed = time.time() - _turn_start
        # 硬超时：node_timeout_sec 是边界约束，超过必须停（不变量 I-2）
        if elapsed > node_timeout_sec:
            emit_event(hook_events.COORDINATOR_MAX_ITERATIONS, {
                "round": round_i + 1,
                "max_rounds": max_rounds,
                "elapsed_s": round(elapsed, 1),
                "node_timeout_sec": node_timeout_sec,
                "reason": "node_timeout",
            }, level="WARN")
            try:
                databus.response_push("coordinator 节点超时，正在汇总…\n",
                                    type="progress", source="coordinator",
                                    status="running")
            except Exception as exc:
                print(f"[flow]   WARN: timeout progress push failed: {exc}", file=sys.stderr)
            # 输出最后一轮的内容作为兜底响应
            response_text = messages[-1].get("content", "") if messages else ""
            # 修复 2026-08-14:超时兜底同样追加产物块(与正常最终响应/llm_error 一致,
            # 原 v2 此处丢失已收集产物)
            response_text = _append_artifacts_section(response_text, generated_artifacts)
            output = {"response": response_text, "status": "node_timeout"}
            print(json.dumps(output, ensure_ascii=False))
            put_e2e_response(databus, response_text)
            save_session_messages(recorder, messages)
            print(f"[timing] node_timeout total={time.time()-_t0:.1f}s", file=sys.stderr)
            return

        # ── Token budget：超阈值压缩 ──
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

        # ── 轮次提醒 ──
        reminder = build_round_reminder(
            round_i=round_i,
            max_rounds=max_rounds,
            elapsed=time.time() - _t0,
            node_timeout_sec=node_timeout_sec,
            custom_hint="本轮必须输出，可以是任务结论或当前执行结果",
        )
        if reminder:
            messages.append(reminder)

        # ── LLM 调用 ──
        _t_llm_start = time.time()
        try:
            if databus.is_connected():
                databus.response_push("coordinator 决策中…\n", type="progress",
                                    source="coordinator", status="running")
        except Exception as exc:
            print(f"[flow]   WARN: coordinator progress push failed: {exc}", file=sys.stderr)

        with emit_span(
            SpanKind.LLM, hook_events.SPAN_COORDINATOR_LLM_ROUND,
            tags={
                "round": str(round_i + 1),
                "messages": str(len(messages)),
                "model": model_name,
                "stream": str(stream),
                "trigger_location": "python_direct",
                "coordinator_version": "v2",
            },
        ) as _llm_span:
            _llm_span.set_tag("dialogue_id", _dialogue_id() or "")
            # ── hooks: LLM 调用前挂载点（注入 messages / 阻断本轮，v2 agent_loop）──
            _llm_ctx = AgentContext(messages=messages)
            hook_trigger(hook_mounts.COORDINATOR_V2_BEFORE_LLM_CALL, {
                "round": round_i + 1,
                "model": model_name,
                "messages_count": len(messages),
            }, can_intervene=True, ctx=_llm_ctx)
            if _llm_ctx.blocked:
                # 阻断本轮 LLM 调用：直接走最终响应出口（无 tool_call 语义）
                raw = {"choices": [{"message": {"role": "assistant",
                                                "content": "（LLM 调用已被 hook 阻断）"}}]}
            else:
                raw = call_llm(
                    messages, tools=available_tools,
                    model=model_name,
                    base_url=base_url,
                    api_key=api_key,
                    max_completion_tokens=max_completion_tokens,
                    stream=stream,
                    source="coordinator",
                    round_i=round_i + 1,
                    extra_body={
                        "temperature": AgentConfig.llm_temperature(),
                        "top_p": AgentConfig.llm_top_p(),
                        "frequency_penalty": AgentConfig.llm_frequency_penalty(),
                        "presence_penalty": AgentConfig.llm_presence_penalty(),
                        "top_k": AgentConfig.llm_top_k() or None,
                        "stop": AgentConfig.llm_stop() or None,
                    }
                )
            _t_llm_end = time.time()
            print(f"[timing] llm_call_end={_t_llm_end-_t0:.1f}s (llm={_t_llm_end-_t_llm_start:.1f}s)",
                  file=sys.stderr)
            # ── hooks: LLM 调用后挂载点（检查/改写决策，v2）──
            _llm_call_id = raw.get("_agentflow_call_id", "")
            hook_trigger(hook_mounts.COORDINATOR_V2_AFTER_LLM_CALL, {
                "round": round_i + 1,
                "call_id": _llm_call_id,
                "raw_choices": raw.get("choices", [{}])[0] if raw.get("choices") else {},
            }, ctx=_llm_ctx)
            _effective_response = recorder.record_effective_response(_llm_call_id, raw)
            hook_trigger(hook_events.COORDINATOR_LLM_RESULT, {
                "round": round_i + 1,
                "call_id": _llm_call_id,
                "response": _effective_response,
            }, ctx=_llm_ctx)

            # observability: log raw LLM response
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
                "round": str(round_i + 1),
                "finish_reason": _obs_finish,
                "tool_call_count": str(_obs_tc_count),
                "tool_call_names": ",".join(_obs_tc_names),
                "content_preview": _obs_content_preview,
                "llm_duration_s": str(round(_t_llm_end - _t_llm_start, 1)),
                "total_duration_s": str(round(_t_llm_end - _t0, 1)),
            })
            emit_event(hook_events.COORDINATOR_LLM_RESULT, {
                "round": round_i + 1,
                "finish_reason": _obs_finish,
                "tool_call_count": _obs_tc_count,
                "tool_call_names": _obs_tc_names,
                "content_preview": _obs_content_preview,
                "llm_duration_s": round(_t_llm_end - _t_llm_start, 1),
                "total_duration_s": round(_t_llm_end - _t0, 1),
            }, duration_ms=(_t_llm_end - _t_llm_start) * 1000)

            # ── LLM 错误兜底 ──
            if raw.get("_llm_error"):
                err_msg = raw.get("error", "LLM call failed")
                _llm_span.set_status(SpanStatus.ERROR, err_msg)
                _llm_span.add_event(hook_events.COORDINATOR_LLM_ERROR, {
                    "round": str(round_i + 1),
                    "error": err_msg,
                    "duration_s": str(round(_t_llm_end - _t0, 1)),
                })
                emit_event(hook_events.COORDINATOR_LLM_ERROR, {
                    "round": round_i + 1,
                    "error": err_msg,
                    "duration_s": round(_t_llm_end - _t0, 1),
                }, level="ERROR")
                # 修复 2026-08-14:传已收集产物,LLM error 时产物不丢失(v1 同款)
                response_text = llm_error_response(json.dumps(generated_artifacts))
                output = {"response": response_text, "status": "llm_error"}
                print(json.dumps(output, ensure_ascii=False))
                put_e2e_response(databus, response_text)
                save_session_messages(recorder, messages)
                print(f"[timing] llm_error total={time.time()-_t0:.1f}s", file=sys.stderr)
                return

        # ── 检测 LLM 返回空业务结果（不依赖 token 计数，与 v1 同款）──
        # v2 修复 2026-08-14:原 v2 缺失此检测,LLM 空响应会被当成正常最终响应
        # 输出空字符串,用户/前端无任何反馈。
        # 复用 coordinator_common.is_empty_llm_response(单一真相源 F4,霰弹式收敛)。
        _is_empty_response = is_empty_llm_response(raw)
        if _is_empty_response:
            err_msg = "LLM returned empty response (no content and no tool calls)"
            emit_event(hook_events.COORDINATOR_LLM_ERROR, {
                "round": round_i + 1,
                "error": err_msg,
                "duration_s": round(time.time() - _t0, 1),
            }, level="ERROR")
            response_text = llm_error_response(json.dumps(generated_artifacts))
            output = {"response": response_text, "status": "llm_error"}
            print(json.dumps(output, ensure_ascii=False))
            put_e2e_response(databus, response_text)
            save_session_messages(recorder, messages)
            print(f"[timing] empty_response total={time.time()-_t0:.1f}s", file=sys.stderr)
            return

        # ── 解析 LLM 响应 ──
        choice = raw.get("choices", [{}])[0]
        msg = choice.get("message", {})
        tool_calls = msg.get("tool_calls")
        messages.append(msg)

        # ── 无 tool_call → 最终响应 ──
        if not tool_calls:
            raw_response = msg.get("content", "")
            # strip <think> tags
            think_match = re.search(r"<think>\s*", raw_response)
            if think_match:
                response_text = raw_response[think_match.end():].strip()
            else:
                response_text = raw_response.strip() if raw_response else ""

            # artifacts 收集（与 v1 / skill_executor 同款）
            response_text = _append_artifacts_section(response_text, generated_artifacts)

            emit_event(hook_events.COORDINATOR_FINAL_RESPONSE, {
                "round": round_i + 1,
                "has_tool_call": False,
                "response_length": len(response_text),
                "elapsed_s": round(time.time() - _t0, 1),
                "coordinator_version": "v2",
            })
            # ── hooks: 最终响应前挂载点（改写 response_text，与 v1 同款）──
            _final_ctx = AgentContext(messages=messages)
            _hook_payload = hook_trigger(
                hook_mounts.COORDINATOR_BEFORE_FINAL_RESPONSE,
                {"round": round_i + 1, "response_text": response_text, "has_tool_call": False},
                can_intervene=True,
                ctx=_final_ctx,
            )
            response_text = _hook_payload.get("response_text", response_text)
            hook_trigger(hook_events.AGENT_DIALOGUE_END, {
                "session_id": session_id,
                "dialogue_id": dialogue_id,
                "status": "normal",
                "response_text": response_text,
                "elapsed_s": round(time.time() - _t0, 1),
            }, can_intervene=True, ctx=_final_ctx)
            hook_trigger(hook_mounts.COORDINATOR_AFTER_FINAL_RESPONSE, {
                "round": round_i + 1,
                "response_length": len(response_text),
                "elapsed_s": round(time.time() - _t0, 1),
            }, ctx=_final_ctx)

            print(json.dumps({"response": response_text}, ensure_ascii=False))
            put_e2e_response(databus, response_text, log_timing=True)
            save_session_messages(recorder, messages)
            print(f"[timing] final_output={time.time()-_t0:.1f}s total", file=sys.stderr)
            return

        # ── 有 tool_call → 执行工具，回灌 messages，continue loop ──
        # v2 核心：不 return yield 给框架，而是自己调工具继续 loop

        # tool_call 死循环检测（复用 skill_executor：返回 bool，非 tuple）
        # 与 skill_executor L1639-1647 同款签名构造：history + 本轮 new_signatures
        _new_signatures: list[tuple[str, str]] = []
        for _tc in tool_calls:
            _func = _tc.get("function", {})
            _name = _func.get("name", "")
            try:
                _sig_args = json.dumps(json.loads(_func.get("arguments", "{}")),
                                       ensure_ascii=False, sort_keys=True)
            except (json.JSONDecodeError, TypeError):
                _sig_args = str(_func.get("arguments", "{}"))
            _new_signatures.append((_name, _sig_args))
        if _detect_tool_call_loop(tool_call_history, _new_signatures, threshold=3):
            emit_event(hook_events.COORDINATOR_TOOL_LOOP_RETRY, {
                "round": round_i + 1,
                "reason": "same_tool_call_signatures_3_rounds",
                "tool_calls": [tc.get("function", {}).get("name") for tc in tool_calls],
            }, level="WARN")
            # 强制 LLM 输出最终响应：注入 reminder
            messages.append({
                "role": "user",
                "content": "检测到工具调用死循环（连续 3 轮相同签名）。请基于已有工具结果直接输出最终响应，不要再调工具。",
            })
            # 不调工具，直接下一轮让 LLM 收敛
            continue

        # 记录 tool_call 签名到 history（死循环检测用）
        round_signatures: list[tuple[str, str]] = []
        for tc in tool_calls:
            func = tc.get("function", {})
            name = func.get("name", "")
            args_str = func.get("arguments", "{}")
            try:
                args_preview = json.dumps(json.loads(args_str), ensure_ascii=False)[:200]
            except (json.JSONDecodeError, TypeError):
                args_preview = args_str[:200]
            round_signatures.append((name, args_preview))
        tool_call_history.append(round_signatures)
        # 只保留最近 10 轮（避免 history 无限增长）
        if len(tool_call_history) > 10:
            tool_call_history.pop(0)

        # 执行每个 tool_call —— 混合批次调度（只读/可并行独立写并行、写串行，批次间保序）
        # 2026-08-16：coordinator v2 接入 execute_in_batches（与 skill_executor L907-942 同款），
        # 两个 generate.py（parallel-write，写不同新文件互不覆盖）可并行执行，图生成
        # 9.3s 串行 → ~4.8s 并行；副作用（F7.1 日志、产物收集、进度推送、消息回灌）
        # 在此处按原始顺序串行处理（tool_policies §2.3）。
        def _execute_tool_call(tc):
            func = tc.get("function", {})
            name = func.get("name", "")
            args_str = func.get("arguments", "{}")
            try:
                args = json.loads(args_str)
            except json.JSONDecodeError:
                args = {}
            # 参数校验（复用 skill_executor）
            is_valid, validation_msg = validate_tool_calls({"tool_calls": [tc]})
            if not is_valid:
                return name, args, json.dumps(
                    {"error": f"invalid tool args: {validation_msg}"}, ensure_ascii=False
                ), -1.0
            # 执行工具（复用 _LocalToolRegistry.execute，走沙箱调度）
            _t_tool_start = time.monotonic()
            try:
                result_str = tool_registry.execute(name, args)
            except Exception as exc:
                result_str = json.dumps({"error": f"Tool '{name}' failed: {exc}"}, ensure_ascii=False)
            _dur_ms = (time.monotonic() - _t_tool_start) * 1000
            return name, args, result_str, _dur_ms

        _tool_results = execute_in_batches(tool_calls, _execute_tool_call)
        for tc, (name, args, result_str, _dur_ms) in zip(tool_calls, _tool_results):
            # F7.1: 工具执行的原始输入必须可追溯（bash 的 command 是定位"探索性探测
            # vs 真实工具"的关键证据，只记 name/duration 无法区分）；F7.2: base64
            # 图片数据字段级单独截断为元信息，避免日志膨胀。校验失败（_dur_ms=-1）不打日志。
            if _dur_ms >= 0:
                _args_log = json.dumps(args, ensure_ascii=False)
                _args_log = re.sub(
                    r"data:image/[a-z0-9+]+;base64,[A-Za-z0-9+/=]+",
                    lambda m: f"data:image/*;base64,<len={len(m.group(0))}>",
                    _args_log,
                )
                print(f"[timing] tool_exec name={name} duration_ms={_dur_ms:.1f} args={_args_log}",
                      file=sys.stderr)

            # 收集工具产生的 artifacts（复用 skill_executor）
            new_artifacts = extract_artifact_paths(result_str)
            if new_artifacts:
                new_artifacts = _ensure_artifacts_in_package(new_artifacts)
                for art in new_artifacts:
                    if art not in generated_artifacts:
                        generated_artifacts.append(art)

            # 进度推送
            try:
                if databus.is_connected():
                    databus.response_push(
                        f"正在执行工具 {name}\n", type="progress",
                        source="coordinator", skill=name, status="running",
                    )
            except Exception as exc:
                print(f"[flow]   WARN: tool progress push failed: {exc}", file=sys.stderr)

            # 回灌 tool result 到 messages（与 skill_executor / v1 同款）
            messages.append({
                "role": "tool",
                "tool_call_id": tc.get("id", name),
                "content": result_str,
            })

        # 工具调用完成 → 继续下一轮 LLM 决策（v2 核心：不 return）
        emit_event(hook_events.COORDINATOR_TOOL_LOOP_RETRY, {
            "round": round_i + 1,
            "tool_loop_round": round_i + 1,
            "tool_calls": [tc.get("function", {}).get("name") for tc in tool_calls],
            "elapsed_s": round(time.time() - _t0, 1),
        })
        # 清掉 response key 防止下轮误读（与 v1 同款）
        if databus.is_connected():
            try:
                databus.delete("response")
            except Exception as exc:
                print(f"[flow] WARN: delete response failed: {exc}", file=sys.stderr)
        continue

    # ── 达到 max_rounds 未收敛 → 强制输出兜底 ──
    emit_event(hook_events.COORDINATOR_MAX_ITERATIONS, {
        "round": max_rounds,
        "max_rounds": max_rounds,
        "elapsed_s": round(time.time() - _t0, 1),
        "reason": "max_rounds_reached",
    }, level="WARN")
    response_text = "已达协调轮次上限，基于已有结果输出。"
    # 尝试从最后几条 messages 提取有用内容
    for m in reversed(messages):
        if m.get("role") in ("assistant", "user") and m.get("content"):
            content = m["content"]
            think_match = re.search(r"<think>\s*", content)
            if think_match:
                content = content[think_match.end():].strip()
            if content:
                response_text = content
                break
    response_text = _append_artifacts_section(response_text, generated_artifacts)
    output = {"response": response_text, "status": "max_rounds_reached"}
    print(json.dumps(output, ensure_ascii=False))
    put_e2e_response(databus, response_text)
    save_session_messages(recorder, messages)
    print(f"[timing] max_rounds total={time.time()-_t0:.1f}s", file=sys.stderr)


# ── 辅助函数（内部） ──
# save_session_messages 已上移 coordinator_common(见顶部 import),不再重复定义。


def _append_artifacts_section(response_text: str, artifacts: list[str]) -> str:
    """追加 artifacts 段到响应文本（与 v1 / skill_executor 同款）。

    v2 修复(2026-08-14)：原简化版不做 URL 重写，只把产物路径列成 markdown 列表。
    实测 sidecar 模拟前端：LLM 回复内联相对路径图片 `assets/outfit-1.png`，
    不做 URL 重写 → bridge artifact_extractor 只匹配 `/api/artifacts/` 前缀
    → UserOutputObject 空 → 前端 broken image。复用 coordinator_common 的
    `append_artifacts_markdown`（rewrite_local_artifact_links 把相对路径重写为
    serve URL + 图片/HTML/下载块追加），单一真相源（F4 拒绝重复实现）。
    """
    if not artifacts:
        return response_text
    # 公共模块已由 execute() 开头的 bind_package_paths 同步 AGENT_PACKAGE_DIR。
    return append_artifacts_markdown(response_text, json.dumps(artifacts))


# ── 入口（直接 python coordinator_agent_loop.py 时触发） ──
if __name__ == "__main__":
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s [%(levelname)s] %(message)s",
        stream=sys.stderr,
        force=True,
    )
    execute()
