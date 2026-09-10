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

import copy
import hashlib
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
    batch_id as _batch_id,
    session_id as _session_id,
    user_id as _user_id,
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
    get_recorder,
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

try:
    from evolution_configured import points as _evolution
except ImportError:
    _evolution = None
if os.environ.get("AF_COORDINATOR_VERSION", "v1") != "v1":
    _evolution = None

from agentflow_runtime.security import install_stderr_redaction, redact_secrets
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
    _resolve_skill_dir,
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

    Used as the ``skill_result`` fallback whenever execution stops before a final
    answer, so the coordinator receives useful intermediate evidence instead of an
    internal user/reminder message (不变量 I-1 不丢失业务数据).

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
    return "\n\n".join(chunks) if chunks else "(skill_executor 在生成最终回答前终止，无可用的 LLM 或工具输出)"


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

def _normalize_after_tool_rewrite(
    tool_calls: list,
    original_results: list,
    rewritten_results,
) -> tuple[list | None, str]:
    """Validate Hook rewrites and normalize them back to execution-order triples."""
    if not isinstance(rewritten_results, (list, tuple)):
        return None, "results must be a list"
    if len(rewritten_results) != len(tool_calls):
        return None, (
            f"results count mismatch: expected {len(tool_calls)}, "
            f"got {len(rewritten_results)}"
        )
    if len(original_results) != len(tool_calls):
        return None, "internal original result count mismatch"

    if all(isinstance(item, dict) for item in rewritten_results):
        expected_by_id = {}
        for tc, original in zip(tool_calls, original_results):
            call_id = tc.get("id")
            if not isinstance(call_id, str) or not call_id:
                return None, "original tool_call_id is missing"
            if call_id in expected_by_id:
                return None, f"duplicate original tool_call_id: {call_id}"
            expected_by_id[call_id] = original

        rewritten_by_id = {}
        for item in rewritten_results:
            call_id = item.get("tool_call_id")
            if not isinstance(call_id, str) or call_id not in expected_by_id:
                return None, f"unknown tool_call_id: {call_id}"
            if call_id in rewritten_by_id:
                return None, f"duplicate tool_call_id: {call_id}"

            expected_name, expected_args, _ = expected_by_id[call_id]
            tool_name = item.get("tool_name")
            arguments = item.get("arguments")
            result = item.get("result")
            if not isinstance(tool_name, str) or tool_name != expected_name:
                return None, f"tool_name mismatch for {call_id}"
            if not isinstance(arguments, dict) or arguments != expected_args:
                return None, f"arguments mismatch for {call_id}"
            if not isinstance(result, str):
                return None, f"result must be a string for {call_id}"
            rewritten_by_id[call_id] = (tool_name, arguments, result)

        return [
            rewritten_by_id[tc["id"]]
            for tc in tool_calls
        ], ""

    if all(
        isinstance(item, (list, tuple)) and len(item) == 3
        for item in rewritten_results
    ):
        normalized = []
        for index, (item, original) in enumerate(
            zip(rewritten_results, original_results)
        ):
            tool_name, arguments, result = item
            expected_name, expected_args, _ = original
            if not isinstance(tool_name, str) or tool_name != expected_name:
                return None, f"legacy tool_name mismatch at index {index}"
            if not isinstance(arguments, dict) or arguments != expected_args:
                return None, f"legacy arguments mismatch at index {index}"
            if not isinstance(result, str):
                return None, f"legacy result must be a string at index {index}"
            normalized.append((tool_name, arguments, result))
        return normalized, ""

    return None, "results must contain only identity records or legacy triples"

def _read_dispatch_manifest(
    databus: DataBusClient, exec_skill_key: str
) -> tuple[dict | None, str]:
    """Return a validated dispatch manifest or a stable preflight error."""
    if not exec_skill_key:
        return None, "missing_exec_skill_key"
    if not databus.is_connected():
        return None, "databus_unavailable"
    try:
        raw = databus.get_global(exec_skill_key)
    except Exception as exc:
        return None, f"manifest_read_error: {exc}"
    if raw in (None, ""):
        return None, "manifest_not_found"
    try:
        manifest = raw if isinstance(raw, dict) else json.loads(raw)
    except (json.JSONDecodeError, TypeError, ValueError) as exc:
        return None, f"manifest_invalid_json: {exc}"
    if not isinstance(manifest, dict):
        return None, "manifest_not_object"

    skill_name = manifest.get("skill_name")
    prompt = manifest.get("prompt")
    load_skills = manifest.get("load_skills", [])
    if not isinstance(skill_name, str) or not skill_name.strip():
        return manifest, "manifest_missing_skill_name"
    if _resolve_skill_dir(skill_name) is None:
        return manifest, "manifest_invalid_skill_name"
    if not isinstance(prompt, str) or not prompt.strip():
        return manifest, "manifest_invalid_prompt"
    if (
        not isinstance(load_skills, list)
        or any(not isinstance(item, str) or not item.strip() for item in load_skills)
    ):
        return manifest, "manifest_invalid_load_skills"
    if any(_resolve_skill_dir(item) is None for item in load_skills):
        return manifest, "manifest_invalid_load_skill_name"
    return manifest, ""


def _finish_dispatch_manifest_error(
    databus: DataBusClient,
    exec_skill_key: str,
    manifest_error: str,
    user_input: str,
    script_start: float,
    skill_name: str = "",
) -> None:
    """Emit a replayable Skill terminal without entering the LLM/Tool loop."""
    error_text = (
        f"Dispatch manifest unavailable for exec_skill_key "
        f"'{exec_skill_key or '(missing)'}': {manifest_error}"
    )
    output = {
        "skill_result": error_text,
        "status": "dispatch_manifest_error",
        "skill_name": skill_name,
        "rounds_used": 0,
        "artifacts_json": "[]",
        "error": manifest_error,
    }
    messages = [{"role": "user", "content": user_input}] if user_input else []
    if _evolution is not None:
        _evolution.point({
            "phase": "after", "kind": "custom",
            "ref": {"run": _dialogue_id(), "key": exec_skill_key},
            "status": "failed", "output": output,
        }, user_id=_user_id())
    ctx = AgentContext(messages=messages, artifacts=[])

    with emit_span(
        SpanKind.SKILL,
        hook_events.SPAN_SKILL_EXECUTOR_EXECUTE,
        tags={
            "skill_name": skill_name,
            "exec_skill_key": exec_skill_key,
            "preflight": "dispatch_manifest_error",
        },
    ) as skill_span:
        skill_span.set_tag("dialogue_id", _dialogue_id() or "")
        load_hooks_from_dir(os.path.join(AGENT_PACKAGE_DIR, "harness", "hooks"))
        start_payload = {
            "session_id": _session_id(),
            "dialogue_id": _dialogue_id(),
            "skill_name": skill_name,
            "exec_skill_key": exec_skill_key,
            "status": "dispatch_manifest_error",
            "error": manifest_error,
            "model": "",
            "max_rounds": 0,
            "tools_count": 0,
        }
        try:
            hook_trigger(
                hook_events.SKILL_EXECUTOR_TRIGGER, start_payload, ctx=ctx
            )
        except Exception as exc:
            print(f"[flow]   WARN: skill start hook failed: {exc}", file=sys.stderr)
        emit_event(
            hook_events.SKILL_EXECUTOR_TRIGGER, start_payload, level="ERROR"
        )
        skill_span.add_event(
            hook_events.SKILL_EXECUTOR_TRIGGER,
            {
                "status": "dispatch_manifest_error",
                "error": manifest_error,
            },
        )

        if databus.is_connected():
            databus.put(
                "skill_result:skill-runner",
                json.dumps(output, ensure_ascii=True),
            )
        print(json.dumps(output, ensure_ascii=False))
        try:
            hook_trigger(
                hook_mounts.SKILL_EXECUTOR_AFTER_SKILL_COMPLETE,
                {
                    "skill_name": skill_name,
                    "round": 0,
                    "status": "dispatch_manifest_error",
                    "skill_result": output["skill_result"],
                    "error": output["error"],
                    "artifacts_count": 0,
                },
                ctx=ctx,
            )
        except Exception as exc:
            print(f"[flow]   WARN: skill end hook failed: {exc}", file=sys.stderr)
        skill_span.set_status(SpanStatus.ERROR, error_text)

    elapsed_ms = (time.monotonic() - script_start) * 1000
    print(
        f"[timing] script_exit duration_ms={elapsed_ms:.1f} "
        f"status=dispatch_manifest_error",
        file=sys.stderr,
    )


# ---------------------------------------------------------------------------
# Main — LLM while-loop with domain tools
# ---------------------------------------------------------------------------

def execute():
    install_stderr_redaction()
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
    manifest, manifest_error = _read_dispatch_manifest(databus, exec_skill_key)
    if manifest_error:
        partial_skill_name = (
            manifest.get("skill_name", "") if isinstance(manifest, dict) else ""
        )
        _finish_dispatch_manifest_error(
            databus, exec_skill_key, manifest_error, user_input, _script_t0,
            partial_skill_name if isinstance(partial_skill_name, str) else "",
        )
        return

    skill_name = manifest["skill_name"]
    skill_prompt = manifest["prompt"]
    requested_load_skill_names = list(manifest.get("load_skills", []))
    load_skill_names = list(requested_load_skill_names)
    dispatch_batch_id = manifest.get("batch_id") or _batch_id()
    dispatch_request_index = manifest.get("request_index")
    dispatch_force = manifest.get("force") is True

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

    # ⑧ Get tools from the primary and explicitly loaded companion skills.
    tool_registry = _LocalToolRegistry(skill_name, load_skill_names)
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
    # Make the manifest-resolved identity visible to nested LLM calls such as
    # hard compaction without requiring a framework-side environment change.
    _previous_skill_name = os.environ.get("AF_SKILL_NAME")
    _previous_llm_timeout = os.environ.get("AF_LLM_TIMEOUT")
    os.environ["AF_SKILL_NAME"] = skill_name
    output = None
    generated_artifacts = []
    round_i = -1
    _skill_end_emitted = False

    def _emit_skill_end_once(ctx=None):
        """Emit the terminal Skill hook exactly once for every structured exit."""
        nonlocal _skill_end_emitted
        if _skill_end_emitted or not isinstance(output, dict):
            return
        _skill_end_emitted = True
        if _evolution is not None:
            _evolution.point({
                "phase": "after", "kind": "custom",
                "ref": {"run": dialogue_id, "key": exec_skill_key},
                "status": "unknown" if output.get("status") == "unhandled_exception" else "completed",
                "output": output,
            }, user_id=_user_id())
        end_ctx = ctx or AgentContext(
            messages=messages,
            artifacts=generated_artifacts,
        )
        end_artifacts = end_ctx.artifacts or []
        try:
            hook_trigger(hook_mounts.SKILL_EXECUTOR_AFTER_SKILL_COMPLETE, {
                "skill_name": skill_name,
                "round": output.get("rounds_used", 0),
                "status": output.get("status", "unknown"),
                "skill_result": output.get("skill_result", ""),
                "error": output.get("error", ""),
                "artifacts_count": len(end_artifacts),
            }, ctx=end_ctx)
        except Exception as exc:
            # 轨迹钩子失败不能改写已经确定的业务终态。
            print(f"[flow]   WARN: skill end hook failed: {exc}", file=sys.stderr)
    try:
        _skill_span.set_tag("dialogue_id", _dialogue_id() or "")

        # ── hooks: 加载 agent 包 harness/hooks（幂等，无 hooks 目录则零开销）──
        load_hooks_from_dir(os.path.join(AGENT_PACKAGE_DIR, "harness", "hooks"))
        # ── hooks: skill 入口（观察；注入/修改在 before_llm_call 挂载点做）──
        hook_trigger(hook_events.SKILL_EXECUTOR_TRIGGER, {
            "session_id": _session_id(),
            "dialogue_id": _dialogue_id(),
            "skill_name": skill_name,
            "skill_prompt": skill_prompt,
            "skill_prompt_preview": skill_prompt[:80] if skill_prompt else "",
            "load_skills": requested_load_skill_names,
            "effective_load_skills": load_skill_names,
            "force": dispatch_force,
            "request_index": dispatch_request_index,
            "exec_skill_key": exec_skill_key,
            "batch_id": dispatch_batch_id,
            "model": model_name,
            "skill_doc_digests": {
                name: hashlib.sha256(content.encode("utf-8")).hexdigest()
                for name, content in skill_docs.items()
            },
            "system_prompt_sha256": hashlib.sha256(
                system_prompt.encode("utf-8")
            ).hexdigest(),
            "task_instruction_sha256": hashlib.sha256(
                task_instruction.encode("utf-8")
            ).hexdigest(),
            "tool_definition_digests": [{
                "name": tool.get("function", {}).get("name", ""),
                "sha256": hashlib.sha256(json.dumps(
                    tool, ensure_ascii=False, sort_keys=True,
                    separators=(",", ":"), default=str,
                ).encode("utf-8")).hexdigest(),
            } for tool in available_tools],
            "max_rounds": max_rounds,
            "tools_count": len(available_tools),
            "tool_names": [t["function"]["name"] for t in available_tools],
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
        try:
            configured_llm_timeout_sec = max(1, int(_previous_llm_timeout or "120"))
        except ValueError:
            configured_llm_timeout_sec = 120
        node_deadline = _script_t0 + float(node_timeout_sec)
        finalize_reserve_sec = min(5.0, max(0.1, float(node_timeout_sec) * 0.05))
        tool_deadline = node_deadline - finalize_reserve_sec
        tool_parameter_specs = {
            tool.get("function", {}).get("name", ""):
                tool.get("function", {}).get("parameters", {}).get("properties", {})
            for tool in available_tools
        }

        def _set_round_llm_timeout_cap() -> bool:
            """Keep all LLM calls inside the remaining node budget."""
            usable_sec = node_deadline - time.monotonic() - finalize_reserve_sec
            if usable_sec < 1.0:
                return False
            timeout_cap = min(configured_llm_timeout_sec, max(1, int(usable_sec)))
            os.environ["AF_LLM_TIMEOUT"] = str(timeout_cap)
            return True

        def _finish_node_timeout(round_number: int) -> None:
            """Write a replayable terminal before the framework hard timeout."""
            nonlocal output
            elapsed_total = time.monotonic() - _script_t0
            emit_event(hook_events.SKILL_EXECUTOR_NODE_TIMEOUT, {
                "round": round_number,
                "max_rounds": max_rounds,
                "skill_name": skill_name,
                "elapsed_s": round(elapsed_total, 1),
                "node_timeout_sec": node_timeout_sec,
            }, level="WARN")
            try:
                databus.response_push(f"{skill_name} 节点超时，正在汇总…\n",
                                      type="progress", skill=skill_name,
                                      round=str(round_number), status="completed",
                                      source="skill_executor")
            except Exception as exc:
                print(f"[flow]   WARN: node_timeout progress push failed: {exc}", file=sys.stderr)
            response_text = _summarize_tool_history(messages)
            output = {
                "skill_result": response_text,
                "status": "node_timeout",
                "skill_name": skill_name,
                "rounds_used": round_number,
                "artifacts_json": json.dumps(generated_artifacts, ensure_ascii=False),
            }
            if databus.is_connected():
                result_json = json.dumps(output, ensure_ascii=True)
                databus.put("skill_result:skill-runner", result_json)
            print(json.dumps(output, ensure_ascii=False))
            elapsed_ms = (time.monotonic() - _script_t0) * 1000
            print(f"[timing] script_exit duration_ms={elapsed_ms:.1f} total", file=sys.stderr)
            _skill_span.set_status(SpanStatus.ERROR, f"node_timeout at round {round_number}")

        completion_budget = AgentConfig.skill_max_completion_tokens()
        recovery_attempts = 0
        max_recovery_attempts = AgentConfig.skill_llm_recovery_attempts()
        referenced_input_artifacts = set(
            extract_artifact_paths(task_instruction)
        )
        generated_artifacts = []
        observed_artifact_paths: set[str] = set()
        unresolved_tool_failures: dict[str, dict] = {}
        # tool_call_history: 每轮 tool_call 签名列表，用于检测 LLM 反复调同一 tool 同一参数的死循环
        tool_call_history: list[list[tuple[str, str]]] = []

        _turn_start = time.time()
        for round_i in range(max_rounds):
            if _evolution is not None and not _evolution.allow_loop({
                "ref": {"run": dialogue_id, "key": f"{exec_skill_key}:loop:{round_i + 1}",
                        "parent": exec_skill_key},
                "state": {"last_result": messages[-1] if messages else {}, "artifacts": generated_artifacts},
            }, user_id=_user_id()):
                output = {
                    "skill_result": _summarize_tool_history(messages), "status": "loop_control_stop",
                    "skill_name": skill_name, "rounds_used": round_i,
                    "artifacts_json": json.dumps(generated_artifacts, ensure_ascii=False),
                }
                if databus.is_connected():
                    databus.put("skill_result:skill-runner", json.dumps(output, ensure_ascii=True))
                print(json.dumps(output, ensure_ascii=False))
                return
            elapsed = time.time() - _turn_start
            # Reserve enough time for result, skill.end and span persistence.
            # The cap also applies to hard-compaction LLM calls in this process.
            if not _set_round_llm_timeout_cap():
                _finish_node_timeout(round_i + 1)
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
                    # Compaction may itself call an LLM, so refresh the cap
                    # immediately before the main round request.
                    if not _set_round_llm_timeout_cap():
                        _llm_span.set_status(SpanStatus.ERROR, "node_timeout before LLM call")
                        _finish_node_timeout(round_i + 1)
                        return
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
                _llm_call_id = raw.get("_agentflow_call_id", "")
                hook_trigger(hook_mounts.SKILL_EXECUTOR_AFTER_LLM_CALL, {
                    "round": round_i + 1,
                    "skill_name": skill_name,
                    "call_id": _llm_call_id,
                    "raw_choices": raw.get("choices", [{}])[0] if raw.get("choices") else {},
                }, ctx=_llm_ctx)
                _effective_response = get_recorder().record_effective_response(
                    _llm_call_id, raw
                )
                hook_trigger(hook_events.SKILL_EXECUTOR_LLM_RESULT, {
                    "round": round_i + 1,
                    "skill_name": skill_name,
                    "call_id": _llm_call_id,
                    "response": _effective_response,
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
                has_recovery_attempt = recovery_attempts < max_recovery_attempts
                has_next_round = round_i + 1 < max_rounds
                if has_recovery_attempt and has_next_round:
                    previous_completion_budget = completion_budget
                    recovery_attempts += 1
                    completion_budget = min(completion_budget * 2, 16384)
                    messages.append({
                        "role": "user",
                        "content": SKILL_RECOVERY_PROMPT,
                    })
                    hook_trigger(hook_events.SKILL_EXECUTOR_RECOVERY_DECISION, {
                        "skill_name": skill_name,
                        "round": round_i + 1,
                        "next_round": round_i + 2,
                        "action": "retry",
                        "reason": invalid_reason,
                        "finish_reason": choice.get("finish_reason", ""),
                        "recovery_attempt": recovery_attempts,
                        "max_recovery_attempts": max_recovery_attempts,
                        "previous_completion_budget": previous_completion_budget,
                        "completion_budget": completion_budget,
                        "stop_reason": "",
                    }, ctx=AgentContext(
                        messages=messages,
                        artifacts=generated_artifacts,
                    ))
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

                recovery_stop_reason = (
                    "max_rounds_reached"
                    if not has_next_round
                    else "recovery_attempts_exhausted"
                )
                hook_trigger(hook_events.SKILL_EXECUTOR_RECOVERY_DECISION, {
                    "skill_name": skill_name,
                    "round": round_i + 1,
                    "next_round": None,
                    "action": "stop",
                    "reason": invalid_reason,
                    "finish_reason": choice.get("finish_reason", ""),
                    "recovery_attempt": recovery_attempts,
                    "max_recovery_attempts": max_recovery_attempts,
                    "previous_completion_budget": completion_budget,
                    "completion_budget": completion_budget,
                    "stop_reason": recovery_stop_reason,
                }, ctx=AgentContext(
                    messages=messages,
                    artifacts=generated_artifacts,
                ))
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
                _tool_call_ordinals = {}
                _tool_call_records = []
                for _ordinal, tc in enumerate(tool_calls, 1):
                    _func = tc.get("function", {})
                    _tool_call_ordinals[id(tc)] = _ordinal
                    _tool_call_records.append({
                        "tool_call_id": tc.get("id", ""),
                        "ordinal": _ordinal,
                        "tool_name": _func.get("name", ""),
                        "arguments_json": _func.get("arguments", "{}"),
                    })
                emit_event(hook_events.SKILL_EXECUTOR_TOOL_CALL, {
                    "round": round_i + 1,
                    "skill_name": skill_name,
                    "tool_names": tool_names,
                    "tool_count": len(tool_calls),
                    "tool_calls_json": json.dumps(
                        _tool_call_records,
                        ensure_ascii=False,
                        separators=(",", ":"),
                        default=str,
                    ),
                }, duration_ms=(time.monotonic() - _llm_t0) * 1000)
                # ── hooks: 工具调用前挂载点（跳过指定工具；ctx 供注入/阻断）──
                _tool_ctx = AgentContext(messages=messages, artifacts=generated_artifacts)
                _tool_payload = hook_trigger(
                    hook_mounts.SKILL_EXECUTOR_BEFORE_TOOL_CALL,
                    {
                        "round": round_i + 1,
                        "skill_name": skill_name,
                        "tool_names": tool_names,
                        "tool_calls": _tool_call_records,
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
                    generated_artifacts = list(_completed_ctx.artifacts)
                    output["artifacts_json"] = json.dumps(generated_artifacts, ensure_ascii=False)
                    if databus.is_connected():
                        result_json = json.dumps(output, ensure_ascii=True)
                        databus.put("skill_result:skill-runner", result_json)
                    print(json.dumps(output, ensure_ascii=False))
                    # ── hooks: skill 完成后挂载点（追加产物/记录）──
                    _emit_skill_end_once(_completed_ctx)
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
                    # Parallel batches contain only bounded built-ins. Shrink
                    # their declared timeout fields to the same node deadline;
                    # serial/unknown tools are additionally interrupted by the
                    # scheduler's POSIX deadline below.
                    timeout_cap = max(1, int(tool_deadline - time.monotonic()))
                    parameter_specs = tool_parameter_specs.get(name, {})
                    for timeout_name in ("timeout", "poll_timeout"):
                        timeout_spec = parameter_specs.get(timeout_name)
                        if not isinstance(timeout_spec, dict):
                            continue
                        requested_timeout = args.get(
                            timeout_name, timeout_spec.get("default")
                        )
                        try:
                            requested_timeout = int(requested_timeout)
                        except (TypeError, ValueError):
                            continue
                        if requested_timeout > timeout_cap:
                            args[timeout_name] = timeout_cap
                    # Log tool call params (truncate to 200 chars)
                    safe_args, _redaction = redact_secrets(args)
                    args_preview = json.dumps(safe_args, ensure_ascii=False)[:200]
                    print(f"[flow]   tool_call: {name} args={args_preview}", file=sys.stderr)
                    if _evolution is not None:
                        result = _evolution.call({
                            "kind": "action", "name": name,
                            "ref": {"run": dialogue_id,
                                    "key": f"{exec_skill_key}:tool:{round_i + 1}:{tc.get('id', _tool_call_ordinals[id(tc)])}",
                                    "parent": exec_skill_key},
                            "actor_id": skill_name, "input": args,
                        }, lambda actual: tool_registry.execute(name, actual),
                            user_id=_user_id(), result_format="json_text")
                        return name, args, result
                    return name, args, tool_registry.execute(name, args)

                _timed_out_tool_call_ids = set()

                def _tool_timeout_result(tc):
                    func = tc.get("function", {})
                    name = func.get("name", "")
                    try:
                        args = json.loads(func.get("arguments", "{}"))
                    except (json.JSONDecodeError, TypeError):
                        args = {}
                    _timed_out_tool_call_ids.add(tc.get("id", name))
                    return name, args, json.dumps({
                        "error": f"Tool '{name}' exceeded the remaining node budget",
                        "status": "node_timeout",
                        "tool": name,
                    }, ensure_ascii=False)

                # ── hooks: 应用 skip 过滤（被跳过的工具不执行，直接回灌提示）──
                _skipped_tool_result_records = []
                _tool_messages_by_ordinal = {}
                if _skip_tools:
                    _kept, _skipped = [], []
                    for tc in tool_calls:
                        _tc_name = tc.get("function", {}).get("name", "")
                        (_skipped if _tc_name in _skip_tools else _kept).append(tc)
                    for tc in _skipped:
                        _tc_func = tc.get("function", {})
                        _tc_name = _tc_func.get("name", "")
                        _skip_result = json.dumps(
                            {"skipped_by_hook": True, "tool": _tc_name},
                            ensure_ascii=False,
                        )
                        _tc_ordinal = _tool_call_ordinals[id(tc)]
                        _tool_messages_by_ordinal[_tc_ordinal] = {
                            "role": "tool",
                            "tool_call_id": tc.get("id", _tc_name),
                            "content": _skip_result,
                        }
                        _skipped_tool_result_records.append({
                            "tool_call_id": tc.get("id", ""),
                            "ordinal": _tool_call_ordinals[id(tc)],
                            "tool_name": _tc_name,
                            "arguments_json": _tc_func.get("arguments", "{}"),
                            "status": "skipped",
                            "result": _skip_result,
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
                        _tool_results = execute_in_batches(
                            tool_calls,
                            _execute_tool_call,
                            deadline_monotonic=tool_deadline,
                            timeout_fn=_tool_timeout_result,
                        )
                        if _timed_out_tool_call_ids:
                            _tool_batch_span.set_status(
                                SpanStatus.ERROR,
                                "tool execution exceeded remaining node budget",
                            )
                # ── hooks: 工具调用后挂载点（检查/改写工具结果）──
                _original_tool_results = [
                    (name, copy.deepcopy(args), result)
                    for name, args, result in _tool_results
                ]
                _tool_rewrite_records = [
                    {
                        "tool_call_id": tc.get("id", ""),
                        "ordinal": _tool_call_ordinals[id(tc)],
                        "tool_name": name,
                        "arguments": copy.deepcopy(args),
                        "result": result,
                    }
                    for tc, (name, args, result) in zip(
                        tool_calls, _original_tool_results
                    )
                ]
                _tool_summary = {
                    "round": round_i + 1,
                    "skill_name": skill_name,
                    "tool_names": tool_names,
                    "results_count": len(_original_tool_results),
                    "results": _tool_rewrite_records,
                }
                _tool_after_payload = hook_trigger(
                    hook_mounts.SKILL_EXECUTOR_AFTER_TOOL_CALL,
                    _tool_summary,
                    can_intervene=True,
                    ctx=_tool_ctx,
                )
                _rewrite_status = "unchanged"
                _rewrite_error = ""
                if not isinstance(_tool_after_payload, dict):
                    _rewrite_status = "ignored_invalid"
                    _rewrite_error = "after_tool_call payload must be an object"
                elif "results" in _tool_after_payload:
                    _normalized_results, _rewrite_error = (
                        _normalize_after_tool_rewrite(
                            tool_calls,
                            _original_tool_results,
                            _tool_after_payload["results"],
                        )
                    )
                    if _rewrite_error:
                        _rewrite_status = "ignored_invalid"
                    else:
                        _tool_results = _normalized_results
                        if _tool_results != _original_tool_results:
                            _rewrite_status = "applied"
                if _rewrite_status == "ignored_invalid":
                    _tool_results = _original_tool_results
                    print(
                        "[flow]   WARN: ignored invalid after_tool_call results "
                        f"rewrite: {_rewrite_error}",
                        file=sys.stderr,
                    )
                # 记录 Hook 改写后的最终结果；这是下一轮 LLM 实际会消费的值。
                _tool_result_records = list(_skipped_tool_result_records)
                for tc, (name, args, result) in zip(tool_calls, _tool_results):
                    _result_data = result
                    if isinstance(result, str):
                        try:
                            _result_data = json.loads(result)
                        except (TypeError, ValueError):
                            _result_data = None
                    _exit_code = (
                        _result_data.get("exit_code")
                        if isinstance(_result_data, dict)
                        else None
                    )
                    _business_status = (
                        str(_result_data.get("status") or "").lower()
                        if isinstance(_result_data, dict)
                        else ""
                    )
                    _result_status = "error" if (
                        isinstance(_result_data, dict)
                        and (
                            bool(_result_data.get("error"))
                            or _result_data.get("ok") is False
                            or _result_data.get("success") is False
                            or _exit_code not in (None, 0, "0")
                            or _business_status in {
                                "failed", "error", "timeout", "cancelled", "interrupted",
                                "tool_error", "node_timeout", "security_policy_blocked",
                                "blocked", "denied", "forbidden", "unauthorized", "rate_limited",
                            }
                        )
                    ) else "completed"
                    _tool_result_records.append({
                        "tool_call_id": tc.get("id", ""),
                        "ordinal": _tool_call_ordinals[id(tc)],
                        "tool_name": name,
                        "arguments": args,
                        "status": _result_status,
                        "result": result,
                    })
                _tool_result_records.sort(key=lambda item: item["ordinal"])
                for _record in _tool_result_records:
                    _record_name = str(_record.get("tool_name") or "")
                    _record_key = json.dumps(
                        [
                            _record_name,
                            _record.get("arguments", _record.get("arguments_json", "")),
                        ],
                        ensure_ascii=False, sort_keys=True, separators=(",", ":"), default=str,
                    )
                    if not _record_name:
                        continue
                    if _record.get("status") == "error":
                        unresolved_tool_failures[_record_key] = _record
                    elif _record.get("status") == "completed":
                        unresolved_tool_failures.pop(_record_key, None)
                _tool_results_json = json.dumps(
                    _tool_result_records,
                    ensure_ascii=False,
                    separators=(",", ":"),
                    default=str,
                )
                _tool_result_payload = {
                    "round": round_i + 1,
                    "skill_name": skill_name,
                    "tool_names": tool_names,
                    "results_count": len(_tool_result_records),
                    "tool_results": _tool_result_records,
                    "after_hook_rewrite_status": _rewrite_status,
                    "after_hook_rewrite_error": _rewrite_error,
                }
                emit_event(hook_events.SKILL_EXECUTOR_TOOL_RESULT, {
                    "round": round_i + 1,
                    "skill_name": skill_name,
                    "results_count": len(_tool_result_records),
                    "tool_results_json": _tool_results_json,
                    "after_hook_rewrite_status": _rewrite_status,
                    "after_hook_rewrite_error": _rewrite_error,
                })
                hook_trigger(
                    hook_events.SKILL_EXECUTOR_TOOL_RESULT,
                    _tool_result_payload,
                    ctx=_tool_ctx,
                )
                for tc, (name, args, result) in zip(tool_calls, _tool_results):
                    # 产物账本只接受 Tool 的结构化字段；任意 stdout/网页/错误文本中的
                    # 路径都不能证明该文件由本轮生成。
                    tool_artifacts = extract_structured_artifacts(result)
                    normalized_tool_artifacts = (
                        _ensure_artifacts_in_package(tool_artifacts)
                        if tool_artifacts else []
                    )
                    observed_artifact_paths.update(tool_artifacts)
                    observed_artifact_paths.update(normalized_tool_artifacts)
                    new_count = 0
                    for artifact in normalized_tool_artifacts:
                        if artifact not in generated_artifacts:
                            generated_artifacts.append(artifact)
                            new_count += 1
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
                    _tool_messages_by_ordinal[_tool_call_ordinals[id(tc)]] = {
                        "role": "tool",
                        "tool_call_id": tc.get("id", name),
                        "content": result,
                    }
                messages.extend(
                    _tool_messages_by_ordinal[ordinal]
                    for ordinal in sorted(_tool_messages_by_ordinal)
                )
                if _timed_out_tool_call_ids:
                    _finish_node_timeout(round_i + 1)
                    return
            else:
                # Task complete → output result
                response_text = msg.get("content", "")
                if not isinstance(response_text, str):
                    response_text = ""
                _empty_final_result = not response_text.strip()
                _has_tool_failure = bool(unresolved_tool_failures)
                _final_status = (
                    "llm_error" if _empty_final_result
                    else "tool_error" if _has_tool_failure
                    else "completed"
                )
                # ── Progress: LLM returned final answer ──
                msg_summary = (
                    f"{skill_name} 返回空结果\n" if _empty_final_result
                    else f"{skill_name} 汇总结果…\n"
                )
                try:
                    databus.response_push(msg_summary, type="progress",
                                          skill=skill_name,
                                          status="error" if _final_status != "completed" else "completed",
                                          source="skill_executor")
                except Exception as exc:
                    print(f"[flow]   WARN: summary progress push failed: {exc}", file=sys.stderr)
                # 最终 LLM 文本不是产物来源事实：只审计其中从未被 Tool 观测的路径，
                # 不把旧文件、输入引用或模型臆测补录为本轮 Artifact。
                if response_text.strip():
                    response_artifacts = extract_all_artifacts(
                        response_text,
                        excluded_text_paths=referenced_input_artifacts,
                    )
                    unseen_response_artifacts = [
                        artifact for artifact in response_artifacts
                        if artifact not in observed_artifact_paths
                    ]
                    if unseen_response_artifacts:
                        print(
                            "[flow]   artifact.ignored_response_text "
                            f"unobserved={len(unseen_response_artifacts)}",
                            file=sys.stderr,
                        )
                output = {
                    "skill_result": response_text,
                    "status": _final_status,
                    "skill_name": skill_name,
                    "rounds_used": round_i + 1,
                    "artifacts_json": json.dumps(generated_artifacts, ensure_ascii=False),
                }
                if _empty_final_result:
                    _empty_error = "LLM returned empty final response (no content and no tool calls)"
                    output["skill_result"] = f"LLM call failed: {_empty_error}"
                    output["error"] = _empty_error
                    emit_event(hook_events.SKILL_EXECUTOR_EMPTY_RESULT, {
                        "round": round_i + 1,
                        "skill_name": skill_name,
                        "tool_calls_in_round": bool(tool_calls),
                        "elapsed_s": round(time.time() - _turn_start, 1),
                    }, level="WARN")
                elif _has_tool_failure:
                    output["error"] = "one or more tool calls ended with an unresolved failure"
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
                generated_artifacts = list(_completed_ctx.artifacts)
                output["artifacts_json"] = json.dumps(generated_artifacts, ensure_ascii=False)
                if databus.is_connected():
                    result_json = json.dumps(output, ensure_ascii=True)
                    databus.put("skill_result:skill-runner", result_json)
                print(json.dumps(output, ensure_ascii=False))
                # ── hooks: skill 完成后挂载点（追加产物/记录）──
                _emit_skill_end_once(_completed_ctx)
                _ms = (time.monotonic() - _script_t0) * 1000
                print(f"[timing] script_exit duration_ms={_ms:.1f} total", file=sys.stderr)
                if _empty_final_result or _has_tool_failure:
                    _failure_detail = (
                        "empty_final_response" if _empty_final_result
                        else "unresolved_tool_failure"
                    )
                    _skill_span.set_status(
                        SpanStatus.ERROR,
                        f"{_final_status} {_failure_detail} at round {round_i+1}",
                    )
                else:
                    _skill_span.set_status(SpanStatus.OK)
                return

        # Max rounds reached
        response_text = _summarize_tool_history(messages)
        emit_event(hook_events.SKILL_EXECUTOR_MAX_ROUNDS_REACHED, {
            "max_rounds": max_rounds,
            "skill_name": skill_name,
            "response_length": len(response_text),
            "elapsed_s": round(time.time() - _turn_start, 1),
        }, level="WARN")
        output = {
            "skill_result": response_text,
            "status": "max_rounds_reached",
            "skill_name": skill_name,
            "rounds_used": max_rounds,
            "artifacts_json": json.dumps(generated_artifacts, ensure_ascii=False),
        }
        if databus.is_connected():
            result_json = json.dumps(output, ensure_ascii=True)
            databus.put("skill_result:skill-runner", result_json)
        print(json.dumps(output, ensure_ascii=False))
        _emit_skill_end_once(
            AgentContext(messages=messages, artifacts=generated_artifacts)
        )
        _ms = (time.monotonic() - _script_t0) * 1000
        print(f"[timing] script_exit duration_ms={_ms:.1f} total", file=sys.stderr)
        _skill_span.set_status(SpanStatus.ERROR, f"max_rounds_reached ({max_rounds})")
    finally:
        _skill_exc_info = sys.exc_info()
        if _skill_exc_info[0] is not None and not _skill_end_emitted:
            output = {
                "skill_result": "",
                "status": "unhandled_exception",
                "skill_name": skill_name,
                "rounds_used": round_i + 1 if round_i >= 0 else 0,
                "error": f"{_skill_exc_info[0].__name__}: {_skill_exc_info[1]}",
            }
        try:
            _emit_skill_end_once()
        finally:
            try:
                _skill_span_cm.__exit__(*_skill_exc_info)
            finally:
                if _previous_skill_name is None:
                    os.environ.pop("AF_SKILL_NAME", None)
                else:
                    os.environ["AF_SKILL_NAME"] = _previous_skill_name
                if _previous_llm_timeout is None:
                    os.environ.pop("AF_LLM_TIMEOUT", None)
                else:
                    os.environ["AF_LLM_TIMEOUT"] = _previous_llm_timeout


if __name__ == "__main__":
    execute()
