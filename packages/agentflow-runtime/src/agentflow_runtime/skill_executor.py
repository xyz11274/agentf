#!/usr/bin/env python3
"""
Universal Skill Executor — parameterized skill task node.

Dispatched by coordinator via run_skills. Receives:
  - Task instruction (skill_prompt:{name}): "what to do"
  - Skill docs (load_skills:{name}): "how to do it"
  - Structured input (exec_skill_key): large/structured data

Executes LLM + skill knowledge + domain tools while-loop, outputs result.

Environment variables (injected by the worker):
  AF_INPUT               — Fallback input (coordinator's original message)
  AF_DIALOGUE_ID         — Current dialogue ID
  AF_SESSION_ID          — Current session ID
  AF_DATABUS_UDS_FILE    — UDS socket for runtime DataBus access
  AF_SKILL_NAME          — Current skill name (injected by framework, runtime.cpp)
  AF_EXEC_SKILL_KEY      — DataBus key for structured input
"""

import json
import logging
import os
import sys
import time

# W1 收敛(2026-08-14):沙箱 workspace 前缀读取从本模块渗漏收敛到 executors factory 层,
# 置于 sys.path 注入前保持 import 在文件顶部(E402 不触发)。

logger = logging.getLogger(__name__)

# ── Add nodes/ to sys.path for local imports ──
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from agentflow_runtime.common.runtime_context import (
    dialogue_id as _dialogue_id,
    exec_skill_key as _exec_skill_key,
    input as _input,
    session_id as _session_id,
)
from agentflow_runtime.common.agent_config import AgentConfig
from agentflow_runtime.databus_client import DataBusClient
from agentflow_runtime.tool_policies import execute_in_batches
from agentflow_runtime.compaction import (
    compress_messages,
    get_compaction_threshold,
    get_model_max_tokens,
)
from agentflow_runtime.llm_call import (
    call_llm,
    resolve_llm_config,
)
from agentflow_runtime.observability import emit_event
from agentflow_runtime import events as hook_events
from agentflow_runtime import mounts as hook_mounts
from agentflow_runtime.context import AgentContext
from agentflow_runtime.hooks import load_hooks_from_dir, trigger as hook_trigger
from agentflow_runtime.trace import emit_span, SpanKind, SpanStatus
from prompts import (
    SKILL_ARTIFACT_AUTO_COLLECT_PROMPT,
    SKILL_ASK_USER_FALLBACK_PROMPT,
    SKILL_FALLBACK_SYSTEM,
    SKILL_NETWORK_FAILURE_PROMPT,
    SKILL_NETWORK_RESOURCE_PROMPT,
    SKILL_RECOVERY_PROMPT,
    SKILL_REPORT_EXECUTION_PROMPT,
    SKILL_SECTION_TEMPLATE,
    SKILL_TOOL_PRIORITY_PROMPT,
)
from agentflow_runtime.round_reminder import build_round_reminder
from agentflow_runtime.harness.mcp import MCP_AVAILABLE, MCPBridge, discover_servers  # noqa: F401, E402

# ── Re-exports from split modules (structural refactor 2026-08-14) ──
# These symbols were extracted into dedicated modules to break up the god-module.
# They are re-imported here so existing `from agentflow_runtime.skill_executor import X`
# call sites continue to work unchanged. Symbols used directly by execute() are
# imported normally; unused re-exports carry noqa: F401.
from agentflow_runtime.artifact_pipeline import (  # noqa: F401, E402
    extract_artifact_paths,
    extract_structured_artifacts,
    extract_all_artifacts,
    _is_user_upload_input,
    _ensure_artifacts_in_package,
    _download_from_sandbox,
    _download_from_sandbox_e2b,
    _upload_artifacts_to_storage,
    _ARTIFACT_JSON_FIELDS,
    _USER_UPLOAD_INPUT_MARKERS,
)
from agentflow_runtime.tool_registry import (  # noqa: F401, E402
    _LocalToolRegistry,
    _load_dir,
    _expand_env,
)
from agentflow_runtime.skills_loader import (  # noqa: F401, E402
    parse_agent_md,
    load_skill_md,
    load_skills_from_list,
)

NODES_DIR = os.path.dirname(os.path.abspath(__file__))
AGENT_PACKAGE_DIR = os.path.dirname(NODES_DIR)
AGENT_MD_PATH = os.path.join(AGENT_PACKAGE_DIR, "agent.md")

# Path to the built-in system tools (shipped with agentflow-runtime package)
_BUILTIN_TOOLS_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "harness", "tools")


# ── §3.5 venv sentinel detection: inject .venv into PATH/PYTHONPATH ──────
# skill_executor is exec'd by shell wrapper which already selected the python
# (either .venv/bin/python3 or fallback system python3). But subprocess.run()
# inside tool execution (e.g. bash tool running "python3 script.py" from SKILL.md)
# resolves "python3" from PATH — which is system python3 by default, NOT .venv.
#
# So we detect the same sentinel the wrapper checked (§3.5), and inject .venv's
# bin/ into PATH + site-packages/ into PYTHONPATH so ALL child subprocesses inherit
# the .venv environment. User skill scripts (weather.py, outfit_vision.py, ...) thus
# run with .venv's python + dependencies WITHOUT needing to know about the framework.
#
# This is idempotent: if sentinel absent (fallback mode), we skip injection and
# child processes use system python3 (which §2.2 Bridge pre-installed agentflow-runtime into).
#
# IMPORTANT: This is a LAZY function (not module-level). At import time, the entry
# file wrapper hasn't yet set the correct AGENT_PACKAGE_DIR (which points to the
# actual agent package dir, NOT the pip package's parent). We call this from
# execute() so the entry-file override of AGENT_PACKAGE_DIR is in effect.
_venv_injected = False

def _inject_venv_env() -> None:
    global _venv_injected
    if _venv_injected:
        return  # idempotent — inject once per process
    _venv_dir = os.path.join(AGENT_PACKAGE_DIR, ".venv")
    _venv_sentinel = os.path.join(_venv_dir, ".agentflow_installed")
    # Cross-platform venv detection: Unix → bin/python3, Windows → Scripts/python.exe
    _venv_python = os.path.join(_venv_dir, "bin", "python3")
    if not os.path.isfile(_venv_python):
        _venv_python = os.path.join(_venv_dir, "Scripts", "python.exe")
    if not (os.path.isfile(_venv_python) and os.path.isfile(_venv_sentinel)):
        return  # fallback mode: system python3 (§2.2 Bridge pre-installed agentflow-runtime)
    # Inject .venv/bin to PATH front so "python3" resolves to .venv's python
    _venv_bin = os.path.join(_venv_dir, "bin")
    if not os.path.isdir(_venv_bin):
        _venv_bin = os.path.join(_venv_dir, "Scripts")
    _existing_path = os.environ.get("PATH", "")
    if _venv_bin not in _existing_path.split(os.pathsep):
        os.environ["PATH"] = _venv_bin + os.pathsep + _existing_path
    # Inject .venv's site-packages to PYTHONPATH (cross-platform: Unix vs Windows layout)
    _site_packages = []
    _lib_candidates = [
        os.path.join(_venv_dir, "lib", "python*", "site-packages"),  # Unix
        os.path.join(_venv_dir, "Lib", "site-packages"),             # Windows
    ]
    import glob as _glob
    for _pat in _lib_candidates:
        _site_packages.extend(_glob.glob(_pat))
    if _site_packages:
        _existing_pp = os.environ.get("PYTHONPATH", "")
        _new_pp = os.pathsep.join(_site_packages)
        if _new_pp not in _existing_pp:
            os.environ["PYTHONPATH"] = _new_pp + (os.pathsep + _existing_pp if _existing_pp else "")
    _venv_injected = True


# ── Helpers for entry-file-aware path resolution ──
def _get_agent_package_dir() -> str:
    return AGENT_PACKAGE_DIR

def _get_skills_base() -> str:
    return os.path.join(AGENT_PACKAGE_DIR, "harness", "skills")

def _get_builtin_tools_dir() -> str:
    """Return path to the built-in system tools directory."""
    return _BUILTIN_TOOLS_DIR

def _get_agent_tools_dir() -> str:
    """Return path to the agent package's local harness/tools (override)."""
    return os.path.join(AGENT_PACKAGE_DIR, "harness", "tools")


# ── Load .env from package root (lazy, respects entry file overrides) ──
# 策略：python-dotenv 首选（官方库更完整），装不上时兜底走 stdlib 零依赖解析器
#（agentflow_runtime.common.env_loader）——避免沙箱 DNS 不可达装不上 dotenv 导致业务断链
_DOTENV_PATH = os.path.join(AGENT_PACKAGE_DIR, ".env")

def _load_dotenv() -> None:
    """Load .env using AGENT_PACKAGE_DIR from globals() (respects entry file overrides).

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
            logger.error("dotenv.load failed: %s", exc)

# _load_dotenv() is called from execute() to respect entry file overrides

# ---------------------------------------------------------------------------
# LLM interaction
# ---------------------------------------------------------------------------


# ---------------------------------------------------------------------------
# Runaway tool-call loop detection helpers
# ---------------------------------------------------------------------------


def _detect_tool_call_loop(
    history: list[list[tuple[str, str]]],
    new_signatures: list[tuple[str, str]],
    threshold: int = 3,
) -> bool:
    """Return True when the same tool_call signatures recur `threshold` rounds in a row.

    A "signature" is ``(tool_name, sorted_json_args)``. LLM陷入"反复调同一 tool 同一
    参数"的死循环时（实测发生过 16 轮调 weather/get_user_profile 不给最终答案），
    连续 ``threshold`` 轮签名完全一致即视为 loop，由调用方强制走完成分支。

    不变量 I-4 幂等性：同一规则重复应用不应产生重复效果——重复 tool_call 即违反。
    """
    if not new_signatures or threshold <= 0:
        return False
    # threshold=1 表示"当前轮出现即触发"——第 1 次就该返回 True，
    # 无需 history。之前漏了这行导致 threshold=1 实际要 2 轮才触发。
    if threshold == 1:
        return True
    new_key = tuple(sorted(new_signatures))
    consecutive = 1
    for past in reversed(history):
        if tuple(sorted(past)) == new_key:
            consecutive += 1
            if consecutive >= threshold:
                return True
        else:
            break
    return False


def _summarize_tool_history(messages: list) -> str:
    """Build a plain-text summary of tool interactions when LLM never gave a final answer.

    Used as the ``skill_result`` fallback when we force-complete on loop detection,
    so the user/coordinator sees *something* useful rather than an empty string
    (不变量 I-1 不丢失业务数据).

    输出必须是**纯文本**而非 Markdown——coordinator 把 ``skill_result`` 直接塞进
    ``response`` key 推给前端，前端按 Markdown 渲染。若这里输出 ``### 工具 X 的输出``
    前端会渲染成 h3 标题而非工具名，造成 UI 错位（实测发生过）。
    所以用 ``【工具 X 的输出】`` 这种方括号标签而非 ``###`` 标题语法。
    """
    chunks: list[str] = []
    for m in messages:
        role = m.get("role", "")
        if role == "tool":
            name = m.get("tool_call_id", "?")
            content = m.get("content", "")
            chunks.append(f"【工具 {name} 的输出】\n{content}")
        elif role == "assistant" and m.get("content"):
            chunks.append(f"【LLM 思考】\n{m['content']}")
    return "\n\n".join(chunks) if chunks else "(skill_executor 在工具调用循环中被强制终止，无可用输出)"


def validate_tool_calls(choice: dict) -> tuple[bool, str]:
    """Reject truncated or syntactically broken tool calls before execution."""
    if choice.get("finish_reason") == "length":
        return False, "completion_truncated"
    for tool_call in choice.get("message", {}).get("tool_calls") or []:
        function = tool_call.get("function", {})
        name = function.get("name", "unknown")
        try:
            arguments = json.loads(function.get("arguments", "{}"))
        except (json.JSONDecodeError, TypeError):
            return False, f"invalid_tool_arguments:{name}"
        if not isinstance(arguments, dict):
            return False, f"invalid_tool_arguments:{name}"
    return True, ""


def remove_invalid_tool_turns(messages: list) -> list:
    """Remove malformed assistant tool calls and their paired tool results."""
    invalid_ids = set()
    cleaned = []
    for message in messages:
        if message.get("role") == "assistant" and message.get("tool_calls"):
            choice = {"finish_reason": "tool_calls", "message": message}
            valid, _ = validate_tool_calls(choice)
            if not valid:
                invalid_ids.update(
                    call.get("id") for call in message.get("tool_calls", [])
                    if call.get("id")
                )
                continue
        if (message.get("role") == "tool" and
                message.get("tool_call_id") in invalid_ids):
            continue
        cleaned.append(message)
    return cleaned


# ---------------------------------------------------------------------------
# Main — LLM while-loop with domain tools
# ---------------------------------------------------------------------------

def execute():
    logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(levelname)s] %(message)s",
                        stream=sys.stderr, force=True)
    _script_t0 = time.monotonic()
    _load_dotenv()  # Load .env (respects entry file overrides of AGENT_PACKAGE_DIR)
    # §3.5 venv sentinel injection — lazy (entry-file AGENT_PACKAGE_DIR override is in effect now)
    _inject_venv_env()
    # ① Read framework-injected environment variables
    user_input     = _input()
    dialogue_id    = _dialogue_id()
    exec_skill_key = _exec_skill_key()

    databus = DataBusClient()
    # Read stream flag from DataBus (persisted by coordinator_decide from bridge input)
    stream_val = databus.get("stream") or ""
    stream = (stream_val == "true")

    # ② Read dispatch manifest from DataBus (key = exec_skill_key, e.g. "dispatch:1")
    #    Manifest: {"skill_name": "echo", "prompt": "...", "load_skills": [...]}
    skill_name = ""
    skill_prompt = ""
    load_skill_names = []

    if exec_skill_key and databus.is_connected():
        raw = databus.get_global(exec_skill_key)
        if raw:
            try:
                manifest = raw if isinstance(raw, dict) else json.loads(raw)
                skill_name = manifest.get("skill_name", "")
                skill_prompt = manifest.get("prompt", "")
                load_skill_names = manifest.get("load_skills", [])
            except (json.JSONDecodeError, TypeError):
                skill_prompt = str(raw)

    # Fallback: if no dispatch manifest, use user_input
    if not skill_name:
        skill_name = "unknown-skill"

    # Ensure own skill doc is loaded
    if skill_name and skill_name not in load_skill_names:
        load_skill_names.insert(0, skill_name)

    # ⑤ Determine final task instruction
    task_instruction = skill_prompt or user_input

    # ⑥ Load skill docs → construct system prompt
    skill_docs = load_skills_from_list(load_skill_names)
    system_parts = []
    for name, content in skill_docs.items():
        system_parts.append(SKILL_SECTION_TEMPLATE.format(name=name, content=content))
    system_prompt = "\n\n---\n\n".join(system_parts) if system_parts else \
                    SKILL_FALLBACK_SYSTEM
    # Append network-failure guard: tells LLM not to retry tools that
    # keep failing, avoiding infinite tool-call loops on network outage.
    system_prompt += "\n\n" + SKILL_NETWORK_FAILURE_PROMPT
    # Append network-resource guard: tells LLM not to retry downloading
    # resources that fail, and to return error info to the user.
    system_prompt += "\n\n" + SKILL_NETWORK_RESOURCE_PROMPT
    # Append artifact-auto-collect instruction: tells LLM that generated
    # artifacts are auto-collected and published; no need for self-hosted
    # HTTP server or file service.
    system_prompt += "\n\n" + SKILL_ARTIFACT_AUTO_COLLECT_PROMPT
    # Append ask_user/render_ui fallback: skill docs may reference session
    # built-in tools that are not in this run's tool list. Degrade to plain-text
    # questioning and signal awaiting_user_input structurally so the coordinator
    # can present the question directly without re-dispatching.
    system_prompt += "\n\n" + SKILL_ASK_USER_FALLBACK_PROMPT
    # Append execution-report requirement: executor must report what it actually
    # did (reads, key checks, external calls) with success/failure, so the
    # coordinator can avoid re-dispatching the same work (重复委派 = 耗时主因).
    system_prompt += "\n\n" + SKILL_REPORT_EXECUTION_PROMPT
    # Append tool-priority principle: prefer specialized tools over generic
    # bash (bash as last resort). 第一性原理抽象（2026-08-10）——通用原则放
    # 系统提示词层一次覆盖所有 skill，避免逐个改 SKILL.md 的霰弹式修改。
    system_prompt += "\n\n" + SKILL_TOOL_PRIORITY_PROMPT

    # ⑦ Construct LLM messages
    messages = [
        {"role": "system", "content": system_prompt},
        {"role": "user",   "content": task_instruction},
    ]

    # ⑧ Get domain tool set from CURRENT skill's scripts/ dir (skill-scoped)
    tool_registry = _LocalToolRegistry(skill_name)
    available_tools = tool_registry.get_tools()

    # ⑨ Read model config with fallback chain
    frontmatter, _ = parse_agent_md(AGENT_MD_PATH)
    model_cfg = frontmatter.get("model", {})
    agent_md_model = model_cfg if isinstance(model_cfg, str) else model_cfg.get("name", "")
    llm_cfg = resolve_llm_config(agent_md_model, node_type="skill")
    model_name = llm_cfg["model"]
    api_key = llm_cfg["api_key"]
    base_url = llm_cfg["base_url"]

    # ⑩ LLM while-loop — max_rounds defined before flow debug to avoid NameError
    max_rounds = AgentConfig.skill_max_iterations()

    # ── SKILL span lifecycle（trigger → completed/各终止出口）──
    _skill_span_cm = emit_span(
        SpanKind.SKILL, hook_events.SPAN_SKILL_EXECUTOR_EXECUTE,
        tags={
            "skill_name": skill_name,
            "exec_skill_key": exec_skill_key,
            "model": model_name,
            "max_rounds": str(max_rounds),
        },
    )
    _skill_span = _skill_span_cm.__enter__()
    try:
        _skill_span.set_tag("dialogue_id", _dialogue_id() or "")

        # ── hooks: 加载 agent 包 harness/hooks（幂等，无 hooks 目录则零开销）──
        load_hooks_from_dir(os.path.join(AGENT_PACKAGE_DIR, "harness", "hooks"))
        # ── hooks: skill 入口（观察；注入/修改在 before_llm_call 挂载点做）──
        hook_trigger(hook_events.SKILL_EXECUTOR_TRIGGER, {
            "session_id": _session_id(),
            "dialogue_id": _dialogue_id(),
            "skill_name": skill_name,
            "skill_prompt_preview": skill_prompt[:80] if skill_prompt else "",
        }, ctx=AgentContext(messages=messages))

        # ── flow debug ──
        emit_event(hook_events.SKILL_EXECUTOR_TRIGGER, {
            "exec_skill_key": exec_skill_key,
            "skill_name": skill_name,
            "skill_prompt_preview": skill_prompt[:80] if skill_prompt else "",
            "load_skills": load_skill_names,
            "tools_available": [t["function"]["name"] for t in available_tools],
            "model": model_name,
            "max_rounds": max_rounds,
        })
        _skill_span.add_event(hook_events.SKILL_EXECUTOR_TRIGGER, {
            "skill_name": skill_name,
            "model": model_name,
            "max_rounds": str(max_rounds),
            "tools_count": str(len(available_tools)),
        })

        # Node-level timeout (injected by framework as AF_NODE_EXECUTION_TIMEOUT);
        # fallback to node default 240s if absent.
        node_timeout_sec = AgentConfig.node_execution_timeout()
        completion_budget = AgentConfig.skill_max_completion_tokens()
        recovery_attempts = 0
        max_recovery_attempts = AgentConfig.skill_llm_recovery_attempts()
        generated_artifacts = extract_artifact_paths(task_instruction)
        generated_artifacts = _ensure_artifacts_in_package(generated_artifacts)
        # tool_call_history: 每轮 tool_call 签名列表，用于检测 LLM 反复调同一 tool 同一参数的死循环
        tool_call_history: list[list[tuple[str, str]]] = []

        _turn_start = time.time()
        for round_i in range(max_rounds):
            elapsed = time.time() - _turn_start
            # ── 硬超时：node_timeout_sec 是边界约束，超过必须停（不变量 I-2）──
            # build_round_reminder 只做"提醒"，没有硬约束；之前节点跑了 3 分钟+ 不停就是这里漏了 break。
            if elapsed > node_timeout_sec:
                emit_event(hook_events.SKILL_EXECUTOR_NODE_TIMEOUT, {
                    "round": round_i + 1,
                    "max_rounds": max_rounds,
                    "skill_name": skill_name,
                    "elapsed_s": round(elapsed, 1),
                    "node_timeout_sec": node_timeout_sec,
                }, level="WARN")
                try:
                    databus.response_push(f"{skill_name} 节点超时，正在汇总…\n",
                                        type="progress", skill=skill_name,
                                        round=str(round_i+1), status="completed",
                                        source="skill_executor")
                except Exception as exc:
                    print(f"[flow]   WARN: node_timeout progress push failed: {exc}", file=sys.stderr)
                response_text = messages[-1].get("content", "") if messages else ""
                output = {
                    "skill_result": response_text,
                    "status": "node_timeout",
                    "skill_name": skill_name,
                    "rounds_used": round_i + 1,
                    "artifacts_json": json.dumps(generated_artifacts, ensure_ascii=False),
                }
                if databus.is_connected():
                    result_json = json.dumps(output, ensure_ascii=True)
                    databus.put("skill_result:skill-runner", result_json)
                print(json.dumps(output, ensure_ascii=False))
                _ms = (time.monotonic() - _script_t0) * 1000
                print(f"[timing] script_exit duration_ms={_ms:.1f} total", file=sys.stderr)
                _skill_span.set_status(SpanStatus.ERROR, f"node_timeout at round {round_i+1}")
                return
            # ── Interrupt flag poll (design §3.3.1) ──────────────────────────
            # daemon 的 handle_dialogue_interrupt 在 cleanup 前写入 global sys flag
            # "/session/{sid}/sys/interrupt:{did}"。每轮顶部轮询，命中则写 interrupted
            # 结果给 coordinator 走正常 cleanup，自己 return。
            # 轮询失败（DataBus 抖动）不阻断主流程（I-INT-5）。
            if databus.is_connected():
                try:
                    _flag = databus.get_global(f"interrupt:{dialogue_id}")
                    if _flag == "true":
                        emit_event(hook_events.SKILL_EXECUTOR_INTERRUPTED, {
                            "round": round_i + 1,
                            "skill_name": skill_name,
                            "elapsed_s": round(time.time() - _turn_start, 1),
                        }, level="WARN")
                        try:
                            databus.response_push(f"{skill_name} 已被取消\n",
                                                type="progress", skill=skill_name,
                                                status="completed", source="skill_executor")
                        except Exception as exc:
                            print(f"[flow]   WARN: interrupt progress push failed: {exc}", file=sys.stderr)
                        output = {
                            "skill_result": "对话已被取消",
                            "status": "interrupted",
                            "skill_name": skill_name,
                            "rounds_used": round_i + 1,
                            "artifacts_json": json.dumps(generated_artifacts, ensure_ascii=False),
                        }
                        if databus.is_connected():
                            databus.put("skill_result:skill-runner",
                                        json.dumps(output, ensure_ascii=True))
                        print(json.dumps(output, ensure_ascii=False))
                        _ms = (time.monotonic() - _script_t0) * 1000
                        print(f"[timing] script_exit duration_ms={_ms:.1f} total", file=sys.stderr)
                        _skill_span.set_status(SpanStatus.CANCELLED, f"interrupted at round {round_i+1}")
                        return
                except Exception as exc:
                    print(f"[flow]   WARN: interrupt flag poll failed: {exc}", file=sys.stderr)
            emit_event(hook_events.SKILL_EXECUTOR_LLM_ROUND_START, {
                "round": round_i + 1,
                "max_rounds": max_rounds,
                "messages": len(messages),
                "model": model_name,
                "tools": bool(available_tools),
                "budget": completion_budget,
                "elapsed_s": round(elapsed, 1),
            })
            # ── LLM span 显式 lifecycle（llm_round_start → llm_result/llm_error）──
            # P2.3 迁移：每轮 LLM 调用一个 span，parent=SKILL span（栈自动维护）。
            # trigger_location=python_direct 标识 Python 侧自行调用（与 C++ worker.cpp 代理调用对等）。
            _llm_span_cm = emit_span(
                SpanKind.LLM, hook_events.SPAN_SKILL_EXECUTOR_LLM_ROUND,
                tags={
                    "round": str(round_i + 1),
                    "model": model_name,
                    "trigger_location": "python_direct",
                },
            )
            _llm_span = _llm_span_cm.__enter__()
            try:
                _llm_span.set_tag("dialogue_id", _dialogue_id() or "")
                _llm_span.add_event(hook_events.SKILL_EXECUTOR_LLM_ROUND_START, {
                    "round": str(round_i + 1),
                    "max_rounds": str(max_rounds),
                    "messages": str(len(messages)),
                    "model": model_name,
                    "tools": str(bool(available_tools)),
                    "budget": str(completion_budget),
                    "elapsed_s": str(round(elapsed, 1)),
                })

                # ── Progress: push round info to frontend ──
                # "第 X 轮：调用 LLM…" → 用户看到"正在等 API"而非"卡了"
                # 末尾 \n：产生端自带换行，外部渲染端拼接多条进度时自然换行（不横排）
                _round_start = time.time()
                progress_msg = f"{skill_name} 第 {round_i+1}/{max_rounds} 轮：思考中…\n"
                # 仅通过 response_push 推流式进度，不再写 DataBus response key
                # （response key 仅供 coordinator 最终输出使用，进度写入会污染最终结果）
                try:
                    databus.response_push(progress_msg, type="progress",
                                          skill=skill_name, round=str(round_i+1),
                                          status="running", source="skill_executor")
                except Exception as exc:
                    print(f"[flow]   WARN: llm_call progress push failed: {exc}", file=sys.stderr)

                # ── Per-round budget reminder (delegated to shared module) ──
                # always=True: 从第 1 轮就注入预算遥测（剩余轮次+已用/剩余时间+进度%），
                # 让 LLM 主动规划收敛而非只在最后 3 轮才察觉预算紧张。
                # 与本节点硬约束（node_timeout break + tool_call_loop detection）互为兜底：
                # 这里是软提示层，硬约束兜底在循环顶部。
                reminder = build_round_reminder(
                    round_i=round_i,
                    max_rounds=max_rounds,
                    elapsed=elapsed,
                    node_timeout_sec=node_timeout_sec,
                    always=True,
                    custom_hint="本轮必须输出：要么调用工具继续推进，要么直接给出 skill_result 文本结果交还 coordinator",
                )
                if reminder:
                    messages.append(reminder)

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
                    node_type="skill",
                )
                if level != "skip":
                    messages = compressed

                _llm_t0 = time.monotonic()
                # ── hooks: skill 内 LLM 调用前挂载点（注入 messages / 阻断本轮）──
                _llm_ctx = AgentContext(messages=messages, artifacts=generated_artifacts)
                hook_trigger(hook_mounts.SKILL_EXECUTOR_BEFORE_LLM_CALL, {
                    "round": round_i + 1,
                    "skill_name": skill_name,
                    "model": model_name,
                    "messages_count": len(messages),
                }, can_intervene=True, ctx=_llm_ctx)
                if _llm_ctx.blocked:
                    # 阻断本轮 LLM 调用：直接走完成分支（无 tool_call 语义）
                    raw = {"choices": [{"message": {"role": "assistant", "content": "（LLM 调用已被 hook 阻断）"}}]}
                    _llm_blocked = True
                else:
                    raw = call_llm(
                        messages, tools=available_tools,
                        model=model_name,
                        base_url=base_url,
                        api_key=api_key,
                        max_completion_tokens=completion_budget,
                        stream=stream,
                        source="skill_executor",
                        skill=skill_name,
                        round_i=round_i + 1,
                        extra_body={
                            "temperature": AgentConfig.skill_llm_temperature(),
                            "top_p": AgentConfig.skill_llm_top_p(),
                            "frequency_penalty": AgentConfig.skill_llm_frequency_penalty(),
                            "presence_penalty": AgentConfig.skill_llm_presence_penalty(),
                            "top_k": AgentConfig.skill_llm_top_k() or None,
                            "stop": AgentConfig.skill_llm_stop() or None,
                        })
                _llm_ms = (time.monotonic() - _llm_t0) * 1000
                print(f"[timing] llm_round round={round_i+1} duration_ms={_llm_ms:.1f}",
                      file=sys.stderr)
                # ── hooks: skill 内 LLM 调用后挂载点（检查/改写响应）──
                hook_trigger(hook_mounts.SKILL_EXECUTOR_AFTER_LLM_CALL, {
                    "round": round_i + 1,
                    "skill_name": skill_name,
                    "raw_choices": raw.get("choices", [{}])[0] if raw.get("choices") else {},
                }, ctx=_llm_ctx)

                if raw.get("_llm_error"):
                    err_msg = raw.get("error", "LLM call failed")
                    emit_event(hook_events.SKILL_EXECUTOR_LLM_ERROR, {
                        "round": round_i + 1,
                        "error": err_msg,
                        "elapsed_s": round(time.time() - _turn_start, 1),
                    }, level="ERROR")
                    # Transport-layer retry is handled INSIDE call_llm (SSL/DNS/timeout).
                    # If we reach here, call_llm has exhausted its retries — the network
                    # is genuinely down. Do NOT rewrite messages (the conversation is
                    # fine; the failure was transport-only). Just bail out with a
                    # structured result so the framework can re-trigger later.
                    output = {
                        "skill_result": f"LLM call failed: {err_msg}",
                        "status": "llm_error",
                        "skill_name": skill_name,
                        "rounds_used": round_i + 1,
                        "error": err_msg,
                        "artifacts_json": json.dumps(generated_artifacts, ensure_ascii=False),
                    }
                    if databus.is_connected():
                        result_json = json.dumps(output, ensure_ascii=True)
                        databus.put("skill_result:skill-runner", result_json)
                    print(json.dumps(output, ensure_ascii=False))
                    _ms = (time.monotonic() - _script_t0) * 1000
                    print(f"[timing] script_exit duration_ms={_ms:.1f} total", file=sys.stderr)
                    _llm_span.set_status(SpanStatus.ERROR, f"llm_error at round {round_i+1}: {err_msg}")
                    _skill_span.set_status(SpanStatus.ERROR, f"llm_error at round {round_i+1}: {err_msg}")
                    return

                # ── 检测 LLM 返回空内容（API 成功但未实际处理请求） ──
                _raw_usage = raw.get("usage", {}) or {}
                _raw_choice = raw.get("choices", [{}])[0]
                _raw_msg = _raw_choice.get("message", {})
                _raw_content = _raw_msg.get("content", "")
                _raw_finish = _raw_choice.get("finish_reason", "")
                _raw_tc = _raw_msg.get("tool_calls")
                _is_empty_response = (
                    not _raw_content
                    and not _raw_tc
                    and _raw_finish == "stop"
                    and _raw_usage.get("prompt_tokens", 0) == 0
                    and _raw_usage.get("completion_tokens", 0) == 0
                )
                if _is_empty_response:
                    err_msg = "LLM returned empty response (zero tokens, no content, finish=stop)"
                    emit_event(hook_events.SKILL_EXECUTOR_LLM_ERROR, {
                        "round": round_i + 1,
                        "error": err_msg,
                        "elapsed_s": round(time.time() - _turn_start, 1),
                    }, level="ERROR")
                    output = {
                        "skill_result": f"LLM call failed: {err_msg}",
                        "status": "llm_error",
                        "skill_name": skill_name,
                        "rounds_used": round_i + 1,
                        "error": err_msg,
                        "artifacts_json": json.dumps(generated_artifacts, ensure_ascii=False),
                    }
                    if databus.is_connected():
                        result_json = json.dumps(output, ensure_ascii=True)
                        databus.put("skill_result:skill-runner", result_json)
                    print(json.dumps(output, ensure_ascii=False))
                    _ms = (time.monotonic() - _script_t0) * 1000
                    print(f"[timing] script_exit duration_ms={_ms:.1f} total", file=sys.stderr)
                    _llm_span.set_status(SpanStatus.ERROR, f"llm_error empty_response at round {round_i+1}")
                    _skill_span.set_status(SpanStatus.ERROR, f"llm_error empty_response at round {round_i+1}")
                    return

                # ── LLM span 正常完成：拿到 choice 后闭合（llm_result 落 span events）──
                choice = raw.get("choices", [{}])[0]
                msg = choice.get("message", {})
                tool_calls = msg.get("tool_calls")
                _llm_span.set_status(SpanStatus.OK)
                _llm_span.add_event(hook_events.SKILL_EXECUTOR_LLM_RESULT, {
                    "round": str(round_i + 1),
                    "finish_reason": choice.get("finish_reason", "?"),
                    "tool_call_count": str(len(tool_calls or [])),
                })
            finally:
                _llm_span_cm.__exit__(*sys.exc_info())

            valid_tool_calls, invalid_reason = validate_tool_calls(choice)
            if not valid_tool_calls:
                # SEMANTIC recovery (truncated tool args / invalid JSON) — this is
                # the legitimate outer-loop recovery: rewrite messages to ask the
                # LLM for shorter, valid tool args. Distinct from transport retry.
                if recovery_attempts < max_recovery_attempts:
                    recovery_attempts += 1
                    completion_budget = min(completion_budget * 2, 16384)
                    messages.append({
                        "role": "user",
                        "content": SKILL_RECOVERY_PROMPT,
                    })
                    print(
                        f"[flow]   recovering {invalid_reason} attempt="
                        f"{recovery_attempts}/{max_recovery_attempts} "
                        f"budget={completion_budget}",
                        file=sys.stderr,
                    )
                    try:
                        databus.response_push(f"{skill_name} 正在恢复第 {round_i+1} 轮…\n",
                                              type="progress", skill=skill_name,
                                              round=str(round_i+1), status="running",
                                              source="skill_executor")
                    except Exception as exc:
                        print(f"[flow]   WARN: recover progress push failed: {exc}", file=sys.stderr)
                    continue

                output = {
                    "skill_result": f"Tool call recovery exhausted: {invalid_reason}",
                    "status": "tool_call_error",
                    "skill_name": skill_name,
                    "rounds_used": round_i + 1,
                    "error": invalid_reason,
                    "artifacts_json": json.dumps(generated_artifacts, ensure_ascii=False),
                }
                if databus.is_connected():
                    result_json = json.dumps(output, ensure_ascii=True)
                    databus.put("skill_result:skill-runner", result_json)
                print(json.dumps(output, ensure_ascii=False))
                _ms = (time.monotonic() - _script_t0) * 1000
                print(f"[timing] script_exit duration_ms={_ms:.1f} total", file=sys.stderr)
                _skill_span.set_status(
                    SpanStatus.ERROR,
                    f"tool_call_error at round {round_i+1}: {invalid_reason}",
                )
                return

            if tool_calls:
                tool_names = [tc.get("function", {}).get("name", "?") for tc in tool_calls]
                emit_event(hook_events.SKILL_EXECUTOR_TOOL_CALL, {
                    "round": round_i + 1,
                    "tool_names": tool_names,
                    "tool_count": len(tool_calls),
                }, duration_ms=(time.monotonic() - _llm_t0) * 1000)
                # ── hooks: 工具调用前挂载点（跳过指定工具；ctx 供注入/阻断）──
                _tool_ctx = AgentContext(messages=messages, artifacts=generated_artifacts)
                _tool_payload = hook_trigger(
                    hook_mounts.SKILL_EXECUTOR_BEFORE_TOOL_CALL,
                    {
                        "round": round_i + 1,
                        "skill_name": skill_name,
                        "tool_names": tool_names,
                        "skip_tool_names": [],
                    },
                    can_intervene=True,
                    ctx=_tool_ctx,
                )
                _skip_tools = set(_tool_payload.get("skip_tool_names") or [])
                if _tool_ctx.blocked:
                    # ctx.blocked 阻断 = 跳过本轮全部工具
                    _skip_tools = set(tool_names)
                # ── Progress: tool call details ──
                msg_tool = f"{skill_name} → 正在调用 {', '.join(tool_names)}\n"
                try:
                    databus.response_push(msg_tool, type="progress",
                                          skill=skill_name, round=str(round_i+1),
                                          status="running", source="skill_executor")
                except Exception as exc:
                    print(f"[flow]   WARN: tool_call progress push failed: {exc}", file=sys.stderr)
                messages.append(msg)
                # ── Runaway detection: track tool_call signatures to detect loops ──
                # LLM 陷入"反复调同一 tool 同一参数"时显式打断，避免耗光 max_rounds 才停。
                # 不变量 I-4 幂等性：同一规则重复应用不应产生重复效果——重复 tool_call 即违反。
                new_signatures = []
                for tc in tool_calls:
                    func = tc.get("function", {})
                    name = func.get("name", "")
                    args = json.loads(func.get("arguments", "{}"))
                    sig = (name, json.dumps(args, ensure_ascii=False, sort_keys=True))
                    new_signatures.append(sig)
                # 连续 3 轮出现完全相同的 tool_call 签名 → 视为死循环，强制走完成分支
                if _detect_tool_call_loop(tool_call_history, new_signatures, threshold=3):
                    emit_event(hook_events.SKILL_EXECUTOR_TOOL_CALL_LOOP_DETECTED, {
                        "round": round_i + 1,
                        "skill_name": skill_name,
                        "signatures": new_signatures,
                        "elapsed_s": round(time.time() - _turn_start, 1),
                    }, level="WARN")
                    try:
                        databus.response_push(f"{skill_name} 检测到重复调用，正在汇总…\n",
                                              type="progress", skill=skill_name,
                                              status="completed", source="skill_executor")
                    except Exception as exc:
                        print(f"[flow]   WARN: loop_detection progress push failed: {exc}", file=sys.stderr)
                    # 强制走完成分支：把最后一轮 LLM 的 content（若有）当结果
                    response_text = msg.get("content", "") or _summarize_tool_history(messages)
                    output = {
                        "skill_result": response_text,
                        "status": "tool_call_loop_detected",
                        "skill_name": skill_name,
                        "rounds_used": round_i + 1,
                        "artifacts_json": json.dumps(generated_artifacts, ensure_ascii=False),
                    }
                    emit_event(hook_events.SKILL_EXECUTOR_COMPLETED, {
                        "round": round_i + 1,
                        "total_rounds": round_i + 1,
                        "skill_name": skill_name,
                        "status": output["status"],
                        "response_length": len(response_text),
                        "artifacts_count": len(generated_artifacts),
                        "elapsed_s": round(time.time() - _turn_start, 1),
                    })
                    # ── hooks: skill 完成前挂载点（改写返回给 coordinator 的 skill_result；ctx 供注入产物/阻断）──
                    _completed_ctx = AgentContext(messages=messages, artifacts=generated_artifacts)
                    _completed_payload = hook_trigger(
                        hook_mounts.SKILL_EXECUTOR_BEFORE_SKILL_COMPLETE,
                        {
                            "skill_name": skill_name,
                            "round": round_i + 1,
                            "status": output["status"],
                            "skill_result": output["skill_result"],
                        },
                        can_intervene=True,
                        ctx=_completed_ctx,
                    )
                    output["skill_result"] = _completed_payload.get("skill_result", output["skill_result"])
                    if _completed_ctx.artifacts is not generated_artifacts:
                        # ctx.artifacts 被替换/扩展时，回写 generated_artifacts（注入产物语义）
                        generated_artifacts = _completed_ctx.artifacts
                        output["artifacts_json"] = json.dumps(generated_artifacts, ensure_ascii=False)
                    if databus.is_connected():
                        result_json = json.dumps(output, ensure_ascii=True)
                        databus.put("skill_result:skill-runner", result_json)
                    print(json.dumps(output, ensure_ascii=False))
                    # ── hooks: skill 完成后挂载点（追加产物/记录）──
                    hook_trigger(hook_mounts.SKILL_EXECUTOR_AFTER_SKILL_COMPLETE, {
                        "skill_name": skill_name,
                        "round": round_i + 1,
                        "status": output["status"],
                        "artifacts_count": len(generated_artifacts),
                    }, ctx=_completed_ctx)
                    _ms = (time.monotonic() - _script_t0) * 1000
                    print(f"[timing] script_exit duration_ms={_ms:.1f} total", file=sys.stderr)
                    _skill_span.set_status(SpanStatus.ERROR, f"tool_call_loop_detected at round {round_i+1}")
                    return
                tool_call_history.append(new_signatures)

                # ── 工具执行：混合批次调度（只读并行、写串行，批次间保序）──
                # 设计文档 tools/system_tools/2.工具执行并行优化.md §3：
                #   - execute_in_batches 只做"按批次调度调用工具拿结果"（可并行部分并行）
                #   - 副作用（产物收集、_ensure 落盘、消息回灌）在此处按原始顺序串行处理
                #   - pool.map 保序 → tool_results[i] 对应 tool_calls[i]
                def _execute_tool_call(tc):
                    func = tc.get("function", {})
                    name = func.get("name", "")
                    args = json.loads(func.get("arguments", "{}"))
                    # Log tool call params (truncate to 200 chars)
                    args_preview = json.dumps(args, ensure_ascii=False)[:200]
                    print(f"[flow]   tool_call: {name} args={args_preview}", file=sys.stderr)
                    return name, args, tool_registry.execute(name, args)

                # ── hooks: 应用 skip 过滤（被跳过的工具不执行，直接回灌提示）──
                if _skip_tools:
                    _kept, _skipped = [], []
                    for tc in tool_calls:
                        _tc_name = tc.get("function", {}).get("name", "")
                        (_skipped if _tc_name in _skip_tools else _kept).append(tc)
                    for tc in _skipped:
                        _tc_name = tc.get("function", {}).get("name", "")
                        messages.append({
                            "role": "tool",
                            "tool_call_id": tc.get("id", _tc_name),
                            "content": json.dumps({"skipped_by_hook": True, "tool": _tc_name},
                                                  ensure_ascii=False),
                        })
                        print(f"[flow]   tool_call SKIPPED by hook: {_tc_name}", file=sys.stderr)
                    tool_calls = _kept

                if not tool_calls:
                    # 所有工具均被 hook 跳过 → 不执行任何工具，回灌后交给 LLM 重新决策
                    _tool_results = []
                else:
                    with emit_span(
                        SpanKind.TOOL,
                        hook_events.SPAN_SKILL_EXECUTOR_TOOL_BATCH,
                        tags={
                            "round": str(round_i + 1),
                            "tool_names": json.dumps([
                                tc.get("function", {}).get("name", "?")
                                for tc in tool_calls
                            ], ensure_ascii=False),
                            "tool_count": str(len(tool_calls)),
                        },
                    ) as _tool_batch_span:
                        _tool_batch_span.set_tag("dialogue_id", _dialogue_id() or "")
                        _tool_results = execute_in_batches(tool_calls, _execute_tool_call)
                # ── hooks: 工具调用后挂载点（检查/改写工具结果）──
                _tool_summary = {
                    "round": round_i + 1,
                    "skill_name": skill_name,
                    "tool_names": tool_names,
                    "results_count": len(_tool_results),
                }
                _tool_after_payload = hook_trigger(
                    hook_mounts.SKILL_EXECUTOR_AFTER_TOOL_CALL,
                    _tool_summary,
                    ctx=_tool_ctx,
                )
                _rewritten_results = _tool_after_payload.get("results")
                if _rewritten_results is not None:
                    # handler 改写工具结果：仅接受与 tool_calls 等长的三元组列表。
                    # 无效改写保留原结果，避免后续 zip 静默丢失 tool message。
                    _valid_rewrite = (
                        isinstance(_rewritten_results, (list, tuple))
                        and len(_rewritten_results) == len(tool_calls)
                        and all(
                            isinstance(item, (list, tuple)) and len(item) == 3
                            for item in _rewritten_results
                        )
                    )
                    if _valid_rewrite:
                        _tool_results = list(_rewritten_results)
                    else:
                        print(
                            "[flow]   WARN: ignored invalid after_tool_call results rewrite "
                            f"expected={len(tool_calls)} actual="
                            f"{len(_rewritten_results) if isinstance(_rewritten_results, (list, tuple)) else 'non-sequence'}",
                            file=sys.stderr,
                        )
                for tc, (name, args, result) in zip(tool_calls, _tool_results):
                    # Audit: artifacts collected from this tool result (完整清单，F7)
                    # 双通道：结构化 JSON 字段（artifacts/files/outputs/result/path）+ regex 文本扫描
                    tool_artifacts = extract_all_artifacts(result)
                    new_count = 0
                    for artifact in tool_artifacts:
                        if artifact not in generated_artifacts:
                            generated_artifacts.append(artifact)
                            new_count += 1
                    # Relocate artifacts written outside agent package into package outputs/
                    generated_artifacts = _ensure_artifacts_in_package(generated_artifacts)
                    # ── F7: 不截断存储层。完整 tool_result 落 stderr ──
                    # 通用路径必须默认完整（F7）；此处是 daemon 捕获 stderr 的主路径，
                    # 完整内容应同时落独立文件兜底（F7.1），但 stderr 链路本身不截断。
                    print(
                        f"[flow]   tool_result: {name} => {result}",
                        file=sys.stderr,
                    )
                    if tool_artifacts:
                        print(
                            f"[flow]   artifact.from_tool {name} collected={len(tool_artifacts)} "
                            f"new={new_count} paths=" + json.dumps(tool_artifacts, ensure_ascii=False),
                            file=sys.stderr,
                        )
                    messages.append({
                        "role": "tool",
                        "tool_call_id": tc.get("id", name),
                        "content": result,
                    })
            else:
                # Task complete → output result
                # ── Progress: LLM returned final answer ──
                msg_summary = f"{skill_name} 汇总结果…\n"
                try:
                    databus.response_push(msg_summary, type="progress",
                                          skill=skill_name, status="completed",
                                          source="skill_executor")
                except Exception as exc:
                    print(f"[flow]   WARN: summary progress push failed: {exc}", file=sys.stderr)
                response_text = msg.get("content", "")
                # 安全网：LLM 最终回复文本中可能包含工具未捕获的文件路径（如图片路径），
                # 兜底扫描并去重。I-6 边界守卫已在 extract_artifact_paths 中内置。
                # 先通过 _ensure_artifacts_in_package 归一化路径（原始 → outputs/{session_id}/{name}），
                # 再与已归一的 generated_artifacts 比较，避免工具结果中的同文件被重复加入。
                if response_text.strip():
                    response_artifacts = extract_all_artifacts(response_text)
                    if response_artifacts:
                        response_normalized = _ensure_artifacts_in_package(response_artifacts)
                        new_count = 0
                        for artifact in response_normalized:
                            if artifact not in generated_artifacts:
                                generated_artifacts.append(artifact)
                                new_count += 1
                        if new_count:
                            generated_artifacts = _ensure_artifacts_in_package(generated_artifacts)
                            print(
                                f"[flow]   artifact.from_response_text new={new_count} "
                                f"total={len(generated_artifacts)}",
                                file=sys.stderr,
                            )
                output = {
                    "skill_result": response_text,
                    "status": "completed",
                    "skill_name": skill_name,
                    "rounds_used": round_i + 1,
                    "artifacts_json": json.dumps(generated_artifacts, ensure_ascii=False),
                }
                if not response_text.strip():
                    emit_event(hook_events.SKILL_EXECUTOR_EMPTY_RESULT, {
                        "round": round_i + 1,
                        "skill_name": skill_name,
                        "tool_calls_in_round": bool(tool_calls),
                        "elapsed_s": round(time.time() - _turn_start, 1),
                    }, level="WARN")
                # Write result to DataBus for coordinator to read.
                #   skill_result:skill-runner — key matching AF_COMPLETED_SKILL_NAME
                # Coordinator's collect_skill_results reads by completed_skill name ("skill-runner"),
                # so this key MUST be written or coordinator gets empty result → false "not found".
                # Use ensure_ascii=True to avoid DataBus SDK UTF-8 decode failures on Chinese/emoji content.
                emit_event(hook_events.SKILL_EXECUTOR_COMPLETED, {
                    "round": round_i + 1,
                    "total_rounds": round_i + 1,
                    "skill_name": skill_name,
                    "status": output.get("status", "completed"),
                    "response_length": len(response_text),
                    "artifacts_count": len(generated_artifacts),
                    "elapsed_s": round(time.time() - _turn_start, 1),
                }, duration_ms=(time.monotonic() - _script_t0) * 1000)
                # ── hooks: skill 完成前挂载点（改写返回给 coordinator 的 skill_result；ctx 供注入产物/阻断）──
                _completed_ctx = AgentContext(messages=messages, artifacts=generated_artifacts)
                _completed_payload = hook_trigger(
                    hook_mounts.SKILL_EXECUTOR_BEFORE_SKILL_COMPLETE,
                    {
                        "skill_name": skill_name,
                        "round": round_i + 1,
                        "status": output.get("status", "completed"),
                        "skill_result": output["skill_result"],
                    },
                    can_intervene=True,
                    ctx=_completed_ctx,
                )
                output["skill_result"] = _completed_payload.get("skill_result", output["skill_result"])
                if _completed_ctx.artifacts is not generated_artifacts:
                    # ctx.artifacts 被替换/扩展时，回写 generated_artifacts（注入产物语义）
                    generated_artifacts = _completed_ctx.artifacts
                    output["artifacts_json"] = json.dumps(generated_artifacts, ensure_ascii=False)
                if databus.is_connected():
                    result_json = json.dumps(output, ensure_ascii=True)
                    databus.put("skill_result:skill-runner", result_json)
                print(json.dumps(output, ensure_ascii=False))
                # ── hooks: skill 完成后挂载点（追加产物/记录）──
                hook_trigger(hook_mounts.SKILL_EXECUTOR_AFTER_SKILL_COMPLETE, {
                    "skill_name": skill_name,
                    "round": round_i + 1,
                    "status": output.get("status", "completed"),
                    "artifacts_count": len(generated_artifacts),
                }, ctx=_completed_ctx)
                _ms = (time.monotonic() - _script_t0) * 1000
                print(f"[timing] script_exit duration_ms={_ms:.1f} total", file=sys.stderr)
                _skill_span.set_status(SpanStatus.OK)
                return

        # Max rounds reached
        emit_event(hook_events.SKILL_EXECUTOR_MAX_ROUNDS_REACHED, {
            "max_rounds": max_rounds,
            "skill_name": skill_name,
            "response_length": len(messages[-1].get("content", "")) if messages else 0,
            "elapsed_s": round(time.time() - _turn_start, 1),
        }, level="WARN")
        output = {
            "skill_result": messages[-1].get("content", "") if messages else "",
            "status": "max_rounds_reached",
            "skill_name": skill_name,
            "rounds_used": max_rounds,
            "artifacts_json": json.dumps(generated_artifacts, ensure_ascii=False),
        }
        if databus.is_connected():
            result_json = json.dumps(output, ensure_ascii=True)
            databus.put("skill_result:skill-runner", result_json)
        print(json.dumps(output, ensure_ascii=False))
        _ms = (time.monotonic() - _script_t0) * 1000
        print(f"[timing] script_exit duration_ms={_ms:.1f} total", file=sys.stderr)
        _skill_span.set_status(SpanStatus.ERROR, f"max_rounds_reached ({max_rounds})")
    finally:
        _skill_span_cm.__exit__(*sys.exc_info())


if __name__ == "__main__":
    execute()
