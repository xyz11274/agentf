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
  AF_COMPLETED_BATCH_KEY — DataBus key containing an ALL-joined Skill batch
"""

import copy
import hashlib
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
    completed_batch_key as _completed_batch_key,
    completed_skill_name as _completed_skill_name,
    dialogue_id as _dialogue_id,
    input as _input,
    session_id as _session_id,
    user_id as _user_id,
    client_id as _client_id,
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
from agentflow_runtime.security import install_stderr_redaction
from agentflow_runtime.parse_af_input import parse_af_input, parse_stream
from prompts import (
    COORDINATOR_FALLBACK_SYSTEM,
    DECISION_FAILURE_PROMPT,
    DECISION_PARTIAL_SUCCESS_PROMPT,
    DECISION_SUCCESS_PROMPT,
    PROTOCOL_TEMPLATE,
    RUN_SKILLS_TOOL_DEFINITION,
    SKILL_COMPLETION_TEMPLATE,
)
from agentflow_runtime.round_reminder import build_round_reminder

try:
    from evolution_configured import points as _evolution
except ImportError:
    _evolution = None

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
    if not completed_skill:
        return False
    for _s in skill_names:
        name = _s.get("name") if isinstance(_s, dict) else str(_s)
        if name != completed_skill:
            continue
        if force_requested or (isinstance(_s, dict) and _s.get("force") is True):
            continue
        prompt = _s.get("prompt", "") if isinstance(_s, dict) else ""
        if last_prompt is not None and prompt != last_prompt:
            # 同名但 prompt 不同 → 新任务,不算重复
            continue
        return True
    return False

def _completed_dispatch_marker_key(
    dialogue_id: str, skill_name: str, prompt: str
) -> str:
    """Return a deterministic key for one completed name+prompt dispatch."""
    canonical = json.dumps(
        [skill_name, prompt], ensure_ascii=False, separators=(",", ":")
    )
    digest = hashlib.sha256(canonical.encode("utf-8")).hexdigest()
    return f"completed_dispatch:{dialogue_id}:{digest}"


def _remember_completed_dispatch(
    databus: DataBusClient,
    dialogue_id: str,
    skill_name: str,
    prompt: str,
    *,
    exec_skill_key: str = "",
    terminal_status: str = "",
) -> bool:
    """Persist an idempotent completion marker without affecting business flow."""
    if not dialogue_id or not skill_name or not isinstance(prompt, str):
        return False
    marker_key = _completed_dispatch_marker_key(dialogue_id, skill_name, prompt)
    payload = {
        "skill_name": skill_name,
        "prompt": prompt,
        "exec_skill_key": exec_skill_key,
        "terminal_status": terminal_status,
    }
    try:
        written = databus.put_global(
            marker_key, json.dumps(payload, ensure_ascii=False)
        )
    except Exception as exc:
        print(
            f"[flow]   WARN: completed dispatch marker write failed "
            f"key={marker_key}: {exc}",
            file=sys.stderr,
        )
        return False
    if not written:
        print(
            f"[flow]   WARN: completed dispatch marker returned false "
            f"key={marker_key}",
            file=sys.stderr,
        )
    return bool(written)


def _remember_completed_batch_dispatches(
    databus: DataBusClient, dialogue_id: str, batch: dict
) -> int:
    """Resolve exact prompts from dispatch manifests and mark terminal branches."""
    remembered = 0
    for result in batch.get("results", []):
        exec_skill_key = result.get("exec_skill_key", "")
        try:
            raw_manifest = databus.get_global(exec_skill_key)
            manifest = (
                json.loads(raw_manifest)
                if isinstance(raw_manifest, str)
                else raw_manifest
            )
        except (json.JSONDecodeError, TypeError, ValueError) as exc:
            print(
                f"[flow]   WARN: completed dispatch manifest decode failed "
                f"key={exec_skill_key}: {exc}",
                file=sys.stderr,
            )
            continue
        except Exception as exc:
            print(
                f"[flow]   WARN: completed dispatch manifest read failed "
                f"key={exec_skill_key}: {exc}",
                file=sys.stderr,
            )
            continue
        if not isinstance(manifest, dict):
            print(
                f"[flow]   WARN: completed dispatch manifest missing "
                f"key={exec_skill_key}",
                file=sys.stderr,
            )
            continue
        skill_name = manifest.get("skill_name")
        prompt = manifest.get("prompt")
        if not isinstance(skill_name, str) or not isinstance(prompt, str):
            print(
                f"[flow]   WARN: completed dispatch manifest identity invalid "
                f"key={exec_skill_key}",
                file=sys.stderr,
            )
            continue
        if _remember_completed_dispatch(
            databus,
            dialogue_id,
            skill_name,
            prompt,
            exec_skill_key=exec_skill_key,
            terminal_status=str(result.get("skill_status", "")),
        ):
            remembered += 1
    return remembered


def _was_dispatch_completed(
    databus: DataBusClient,
    dialogue_id: str,
    skill_name: str,
    prompt: str,
) -> bool:
    """Check the exact dialogue-scoped completion marker; read failure is open."""
    if not dialogue_id or not skill_name or not isinstance(prompt, str):
        return False
    marker_key = _completed_dispatch_marker_key(dialogue_id, skill_name, prompt)
    try:
        return databus.get_global(marker_key) not in (None, "")
    except Exception as exc:
        print(
            f"[flow]   WARN: completed dispatch marker read failed "
            f"key={marker_key}: {exc}",
            file=sys.stderr,
        )
        return False

def _find_persisted_completed_dispatch(
    databus: DataBusClient,
    dialogue_id: str,
    skill_specs: list,
    force_all: bool,
) -> str:
    """Return the first exact completed dispatch not covered by a force escape."""
    if force_all or not databus.is_connected():
        return ""
    for spec in skill_specs:
        if not isinstance(spec, dict) or spec.get("force") is True:
            continue
        skill_name = spec.get("name", "")
        if _was_dispatch_completed(
            databus, dialogue_id, skill_name, spec.get("prompt", "")
        ):
            return skill_name
    return ""


def _validate_run_skills_args(
    args: Any,
    available_skills: list[str] | None = None,
) -> str:
    """Return an actionable validation error, or an empty string when valid."""
    if not isinstance(args, dict):
        return "arguments must be a JSON object"

    skills = args.get("skills")
    if not isinstance(skills, list) or not skills:
        return "skills must be a non-empty array"
    if "reason" in args and not isinstance(args["reason"], str):
        return "reason must be a string"
    if "force" in args and not isinstance(args["force"], bool):
        return "force must be a boolean"

    allowed = set(available_skills) if available_skills is not None else None
    for index, spec in enumerate(skills):
        path = f"skills[{index}]"
        if not isinstance(spec, dict):
            return f"{path} must be an object"

        name = spec.get("name")
        if not isinstance(name, str) or not name.strip():
            return f"{path}.name must be a non-empty string"
        if allowed is not None and name not in allowed:
            choices = ", ".join(available_skills or []) or "(none)"
            return f"{path}.name '{name}' is unavailable; allowed: {choices}"

        prompt = spec.get("prompt")
        if not isinstance(prompt, str) or not prompt.strip():
            return f"{path}.prompt must be a non-empty string"

        load_skills = spec.get("load_skills", [])
        if (
            not isinstance(load_skills, list)
            or any(not isinstance(item, str) or not item.strip() for item in load_skills)
        ):
            return f"{path}.load_skills must be an array of non-empty strings"
        if allowed is not None:
            for load_index, load_name in enumerate(load_skills):
                if load_name not in allowed:
                    choices = ", ".join(available_skills or []) or "(none)"
                    return f"{path}.load_skills[{load_index}] '{load_name}' is unavailable; allowed: {choices}"

        if "force" in spec and not isinstance(spec["force"], bool):
            return f"{path}.force must be a boolean"
        if "exec_skill_key" in spec and not isinstance(spec["exec_skill_key"], str):
            return f"{path}.exec_skill_key must be a string"

    return ""


def _invalid_run_skills_result(error: str) -> dict:
    """Build the normal Tool result used by the Coordinator correction loop."""
    return {
        "status": "invalid_arguments",
        "batch_id": "",
        "skills": [],
        "exec_skill_keys": [],
        "dispatches": [],
        "rejected": [{
            "request_index": 0,
            "stage": "argument_validation",
            "error": error,
        }],
        "warnings": [],
        "error": error,
        "response": f"run_skills 参数无效：{error}。请修正参数后重新决策。",
    }


def handle_run_skills_call(args: Any, databus: DataBusClient,
                           session_id: str, dialogue_id: str) -> str:
    """Intercept run_skills tool_call → write dispatch info to DataBus → trigger framework.

    All skills are routed through a single 'skill-runner' task.
    Real skill identity is carried by exec_skill_key → DataBus dispatch manifest.
    """
    validation_error = _validate_run_skills_args(args)
    if validation_error:
        return json.dumps(_invalid_run_skills_result(validation_error), ensure_ascii=False)

    skills = args.get("skills", [])
    reason = args.get("reason", "")

    # ① Write per-skill dispatch manifest to DataBus (must be BEFORE next_skills)
    #    Key: dispatch:{N} → value includes the complete invocation identity and
    #    arguments needed by the Skill process and trajectory recorder.
    next_skills_specs = []
    accepted_specs = []
    rejected = []
    dispatch_round = uuid.uuid4().hex[:12]
    batch_id = f"batch:{dialogue_id}:{dispatch_round}"
    for i, spec in enumerate(skills, 1):
        dispatch_key = f"dispatch:{dialogue_id}:{dispatch_round}:{i}"
        dispatch_data = {
            "skill_name": spec["name"],
            "prompt": spec.get("prompt", ""),
            "load_skills": spec.get("load_skills", []),
            "batch_id": batch_id,
            "request_index": i,
            "force": bool(spec.get("force", False)),
        }
        manifest_written = databus.put_global(
            dispatch_key,
            json.dumps(dispatch_data, ensure_ascii=False),
        )
        if not manifest_written:
            rejected.append({
                "name": spec["name"],
                "request_index": i,
                "stage": "manifest_write",
                "error": "DataBus put_global returned false",
            })
            continue

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
            "batch_id": batch_id,
            "force": bool(spec.get("force", False)),
        })
        accepted_specs.append((i, spec, dispatch_key))


    if not accepted_specs:
        return json.dumps({
            "status": "dispatch_failed",
            "batch_id": batch_id,
            "skills": [],
            "exec_skill_keys": [],
            "dispatches": [],
            "rejected": rejected,
            "warnings": [],
            "message": "没有 skill 成功写入 dispatch manifest，请重新决策。",
            "reason": reason,
        }, ensure_ascii=False)

    # ③ Write next_skills → framework takes over scheduling
    if _evolution is not None:
        for _, spec, dispatch_key in accepted_specs:
            _evolution.point({
                "phase": "before", "kind": "custom", "name": "skill",
                "ref": {"run": dialogue_id, "key": dispatch_key},
                "actor_id": spec["name"],
                "input": {"skill_name": spec["name"], "prompt": spec.get("prompt", ""),
                          "load_skills": spec.get("load_skills", [])},
            }, user_id=_user_id())
    next_skills_written = databus.put(
        "next_skills",
        json.dumps(next_skills_specs, ensure_ascii=False),
    )
    if not next_skills_written:
        if _evolution is not None:
            for _, _, dispatch_key in accepted_specs:
                _evolution.point({
                    "phase": "after", "kind": "custom",
                    "ref": {"run": dialogue_id, "key": dispatch_key},
                    "status": "failed", "output": {"error": "next_skills_write_failed"},
                }, user_id=_user_id())
        rejected.extend({
            "name": spec["name"],
            "request_index": request_index,
            "stage": "next_skills_write",
            "error": "DataBus put returned false",
        } for request_index, spec, _ in accepted_specs)
        return json.dumps({
            "status": "dispatch_failed",
            "batch_id": batch_id,
            "skills": [],
            "exec_skill_keys": [],
            "dispatches": [],
            "rejected": rejected,
            "warnings": [],
            "message": "next_skills 提交失败，没有 skill 被框架调度，请重新决策。",
            "reason": reason,
        }, ensure_ascii=False)

    # next_skills 提交成功后再记录 last_prompt，避免未实际调度的 skill
    # 污染下一轮 F2 重复判定。该辅助写入失败不回滚已经提交的调度。
    warnings = []
    for _, spec, _ in accepted_specs:
        prompt_written = databus.put_global(
            f"last_prompt:{dialogue_id}:{spec['name']}",
            spec.get("prompt", ""),
        )
        if not prompt_written:
            warnings.append({
                "name": spec["name"],
                "stage": "last_prompt_write",
                "error": "DataBus put_global returned false",
            })

    # ④ Return confirmation
    accepted_names = [spec["name"] for _, spec, _ in accepted_specs]
    dispatches = [{
        "request_index": request_index,
        "skill_name": spec["name"],
        "prompt": spec.get("prompt", ""),
        "load_skills": list(spec.get("load_skills", [])),
        "force": bool(spec.get("force", False)),
        "exec_skill_key": dispatch_key,
        "batch_id": batch_id,
    } for request_index, spec, dispatch_key in accepted_specs]
    return json.dumps({
        "status": "partially_dispatched" if rejected else "dispatched",
        "batch_id": batch_id,
        "skills": accepted_names,
        "exec_skill_keys": [key for _, _, key in accepted_specs],
        "dispatches": dispatches,
        "rejected": rejected,
        "warnings": warnings,
        "message": f"已调度 {len(accepted_names)} 个 skill，等待执行结果...",
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


def attach_skill_completion_result(
    history: Any,
    completion_message: str,
    batch_id: str = "",
    skill_names: list[str] | None = None,
) -> list[dict]:
    """Attach an asynchronous completion as the result of its run_skills call.

    A successful dispatch already left an assistant tool call and a temporary
    dispatch ACK in session history. Replace that ACK in place so the next LLM
    request contains one protocol-valid tool result instead of a fabricated
    user turn. If history could not be restored, reconstruct the missing pair
    rather than sending an orphan tool message.
    """
    if not isinstance(history, list):
        history = []
    normalized_names = [str(name) for name in (skill_names or []) if name]

    for message in reversed(history):
        if not isinstance(message, dict) or message.get("role") != "tool":
            continue
        try:
            dispatch_ack = json.loads(message.get("content", ""))
        except (json.JSONDecodeError, TypeError):
            continue
        if not isinstance(dispatch_ack, dict) or dispatch_ack.get("status") not in {
            "dispatched", "partially_dispatched",
        }:
            continue
        ack_batch_id = str(dispatch_ack.get("batch_id", ""))
        if batch_id and ack_batch_id != batch_id:
            continue
        message["content"] = json.dumps({
            "status": "execution_completed",
            "batch_id": batch_id or ack_batch_id,
            "skills": normalized_names or dispatch_ack.get("skills", []),
            "dispatch": dispatch_ack,
            "result": completion_message,
        }, ensure_ascii=False)
        return history

    # Recorder/DataBus history may be unavailable after a transient failure.
    # Reconstruct a valid assistant→tool pair so Skill output still stays in
    # the low-trust tool channel and the provider never receives an orphan.
    identity = batch_id or ",".join(normalized_names) or "legacy-skill-completion"
    synthetic_id = f"af_completion_{uuid.uuid5(uuid.NAMESPACE_URL, identity).hex[:16]}"
    history.append({
        "role": "assistant",
        "content": "",
        "tool_calls": [{
            "id": synthetic_id,
            "type": "function",
            "function": {
                "name": "run_skills",
                "arguments": json.dumps({
                    "skills": [
                        {"name": name, "prompt": "", "load_skills": []}
                        for name in normalized_names
                    ],
                    "reason": "restored asynchronous completion",
                }, ensure_ascii=False),
            },
        }],
    })
    history.append({
        "role": "tool",
        "tool_call_id": synthetic_id,
        "content": json.dumps({
            "status": "execution_completed",
            "batch_id": batch_id,
            "skills": normalized_names,
            "result": completion_message,
        }, ensure_ascii=False),
    })
    return history

def _mark_completed_batch_consumed(
    databus: DataBusClient, completed_batch_key: str, max_attempts: int = 3
) -> None:
    """写入 Batch 消费屏障；DataBus 返回 False 时做有界重试。"""
    marker_key = f"{completed_batch_key}:consumed"
    for attempt in range(1, max_attempts + 1):
        if databus.put(marker_key, "true"):
            return
        print(
            f"[flow]   WARN: mark batch consumed returned false "
            f"attempt={attempt}/{max_attempts} key={marker_key}",
            file=sys.stderr,
        )
    raise ValueError(
        f"completed batch '{completed_batch_key}' consumed marker write failed"
    )


def parse_completed_batch(raw_batch: Any, completed_batch_key: str) -> dict:
    """Parse and validate one C++ ALL-join result from the dialogue DataBus."""
    try:
        batch = json.loads(raw_batch) if isinstance(raw_batch, str) else raw_batch
    except (json.JSONDecodeError, TypeError) as exc:
        raise ValueError(
            f"completed batch '{completed_batch_key}' is not valid JSON"
        ) from exc
    if not isinstance(batch, dict) or not isinstance(batch.get("results"), list):
        raise ValueError(
            f"completed batch '{completed_batch_key}' must contain a results array"
        )
    if not batch["results"]:
        raise ValueError(
            f"completed batch '{completed_batch_key}' has an empty results array"
        )
    if not batch.get("batch_id"):
        raise ValueError(
            f"completed batch '{completed_batch_key}' has no batch_id"
        )
    for index, result in enumerate(batch["results"]):
        if not isinstance(result, dict) or not result.get("exec_skill_key"):
            raise ValueError(
                f"completed batch '{completed_batch_key}' result[{index}] "
                "has no exec_skill_key"
            )
    return batch


def build_skill_batch_completion_message(batch: dict) -> str:
    """Build one existing-format completion message for an entire Skill batch."""
    normalized_results = []
    completed_names = []
    all_succeeded = True
    any_succeeded = False
    for result in batch["results"]:
        name = (
            result.get("business_skill_name")
            or result.get("framework_skill_name")
            or result.get("exec_skill_key")
        )
        completed_names.append(name)
        skill_result = result.get("skill_result", "")
        if isinstance(skill_result, str):
            try:
                skill_result = json.loads(skill_result)
            except (json.JSONDecodeError, TypeError):
                pass
        succeeded = result.get("skill_success") is True
        all_succeeded = all_succeeded and succeeded
        any_succeeded = any_succeeded or succeeded
        normalized_results.append({
            "skill": name,
            "exec_skill_key": result.get("exec_skill_key", ""),
            "workflow_execution_id": result.get("workflow_execution_id", ""),
            "workflow_success": result.get("workflow_success") is True,
            "skill_success": succeeded,
            "skill_status": result.get("skill_status", ""),
            "result": skill_result,
            "artifacts_json": result.get("artifacts_json", "[]"),
        })

    if all_succeeded:
        batch_success = "true"
        decision = DECISION_SUCCESS_PROMPT
    elif any_succeeded:
        batch_success = "partial"
        decision = DECISION_PARTIAL_SUCCESS_PROMPT
    else:
        batch_success = "false"
        decision = DECISION_FAILURE_PROMPT
    completed_label = (
        f"batch {batch['batch_id']} [" + ", ".join(completed_names) + "]"
    )
    return SKILL_COMPLETION_TEMPLATE.format(
        completed_skill=completed_label,
        skill_success=batch_success,
        skill_results=json.dumps(normalized_results, ensure_ascii=False, indent=2),
        decision=decision,
    )


def collect_batch_artifacts(batch: dict) -> str:
    """Flatten per-Skill artifact arrays for the existing final-response path."""
    artifacts = []
    for result in batch["results"]:
        raw_artifacts = result.get("artifacts_json", "[]")
        if isinstance(raw_artifacts, str):
            try:
                raw_artifacts = json.loads(raw_artifacts)
            except (json.JSONDecodeError, TypeError):
                raw_artifacts = []
        if isinstance(raw_artifacts, list):
            artifacts.extend(raw_artifacts)
    return json.dumps(artifacts, ensure_ascii=False)


_DIALOGUE_ARTIFACTS_KEY = "_coordinator_artifacts"


def merge_dialogue_artifacts(
    databus: DataBusClient, current_artifacts: Any, max_attempts: int = 3
) -> str:
    """Merge current completion artifacts into a dialogue-scoped stable ledger."""

    def _parse(raw: Any) -> list:
        if isinstance(raw, bytes):
            raw = raw.decode("utf-8", errors="replace")
        if isinstance(raw, str):
            try:
                raw = json.loads(raw)
            except (json.JSONDecodeError, TypeError):
                return []
        return raw if isinstance(raw, list) else []

    previous = []
    connected = databus.is_connected()
    if connected:
        try:
            previous = _parse(databus.get(_DIALOGUE_ARTIFACTS_KEY))
        except Exception as exc:
            print(
                f"[flow]   WARN: read dialogue artifacts failed: {exc}",
                file=sys.stderr,
            )

    merged = []
    seen = set()
    for item in previous + _parse(current_artifacts):
        try:
            identity = json.dumps(
                item, ensure_ascii=False, sort_keys=True, separators=(",", ":")
            )
        except (TypeError, ValueError):
            continue
        if identity in seen:
            continue
        seen.add(identity)
        merged.append(item)

    serialized = json.dumps(merged, ensure_ascii=False)
    if connected:
        for attempt in range(1, max_attempts + 1):
            try:
                if databus.put(_DIALOGUE_ARTIFACTS_KEY, serialized):
                    break
            except Exception as exc:
                print(
                    f"[flow]   WARN: write dialogue artifacts failed: {exc}",
                    file=sys.stderr,
                )
            print(
                f"[flow]   WARN: persist dialogue artifacts returned false "
                f"attempt={attempt}/{max_attempts}",
                file=sys.stderr,
            )
    return serialized


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
    completed_batch_key = _completed_batch_key()

    max_iterations = AgentConfig.coordinator_max_iterations()
    max_completion_tokens = AgentConfig.max_completion_tokens()

    frontmatter, agent_md_body = parse_agent_md(AGENT_MD_PATH)
    agent_definition_sha256 = hashlib.sha256(json.dumps(
        {"frontmatter": frontmatter, "body": agent_md_body},
        ensure_ascii=False, sort_keys=True, separators=(",", ":"), default=str,
    ).encode("utf-8")).hexdigest()
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
    _dialogue_end_emitted = False

    def _emit_dialogue_end_once(status, response_text, ctx=None, can_intervene=False):
        """Emit one dialogue terminal event without changing business output."""
        nonlocal _dialogue_end_emitted
        if _dialogue_end_emitted:
            return
        _dialogue_end_emitted = True
        if _evolution is not None:
            _evolution.point({
                "phase": "after", "kind": "run", "ref": {"run": dialogue_id},
                "output": {"status": status, "response": response_text},
            }, user_id=_user_id())
        end_ctx = ctx or AgentContext()
        try:
            hook_trigger(hook_mounts.AGENT_DIALOGUE_END, {
                "session_id": session_id,
                "dialogue_id": dialogue_id,
                "status": status,
                "response_text": response_text,
                "elapsed_s": round(time.time() - _t0, 1),
            }, can_intervene=can_intervene, ctx=end_ctx)
        except Exception as exc:
            # 轨迹钩子失败不能改变 Coordinator 已确定的最终响应。
            print(f"[flow]   WARN: dialogue end hook failed: {exc}", file=sys.stderr)
    is_completion_trigger = bool(
        completed_batch_key
        or (completed_skill and completed_skill != "dummy_pre_skill")
    )
    databus = DataBusClient()
    is_first_trigger = not is_completion_trigger
    if is_first_trigger:
        current_round = 1
    else:
        raw_round = databus.get("_coordinator_round") if databus.is_connected() else None
        try:
            current_round = int(raw_round) + 1 if raw_round is not None else 1
        except (ValueError, TypeError):
            current_round = 1
    trigger_type = "skill_completion" if is_completion_trigger else "first"
    activation_payload = {
        "session_id": session_id,
        "dialogue_id": dialogue_id,
        "input": raw_input if is_first_trigger else "",
        "trigger_type": trigger_type,
        "completed_skill": completed_skill if is_completion_trigger else "",
        "completed_batch_key": completed_batch_key,
        "skill_success": skill_success,
        "coordinator_version": "v1",
        "code_revision": (
            os.environ.get("AF_CODE_REVISION")
            or os.environ.get("GIT_COMMIT", "")
        ),
        "agent_definition_sha256": agent_definition_sha256,
        "model": model_name,
        "max_iterations": max_iterations,
        "max_completion_tokens": max_completion_tokens,
        "activation_index": current_round,
        "skill_completion_index": max(0, current_round - 1),
    }

    # Dialogue start is emitted once; every event-driven re-entry is a separate
    # Coordinator activation and uses the existing coordinator.trigger event.
    if not is_completion_trigger:
        if _evolution is not None:
            _evolution.point({
                "phase": "before", "kind": "run", "ref": {"run": dialogue_id},
                "conversation_id": _client_id() or dialogue_id, "input": raw_input,
            }, user_id=_user_id())
        hook_trigger(
            hook_mounts.AGENT_DIALOGUE_START,
            dict(activation_payload),
            ctx=AgentContext(),
        )
    hook_trigger(
        hook_events.COORDINATOR_TRIGGER,
        dict(activation_payload),
        ctx=AgentContext(),
    )
    emit_event(hook_events.COORDINATOR_TRIGGER, dict(activation_payload))
    _t1 = time.time()

    recorder = get_recorder()
    completed_batch = None
    completed_batch_message = ""
    completed_skill_names: list[str] = []
    if completed_batch_key:
        try:
            raw_batch = databus.get(completed_batch_key)
            if raw_batch in (None, ""):
                raise ValueError(
                    f"completed batch '{completed_batch_key}' was not found"
                )
            completed_batch = parse_completed_batch(raw_batch, completed_batch_key)
            for result in completed_batch["results"]:
                name = (
                    result.get("business_skill_name")
                    or result.get("framework_skill_name")
                    or result.get("exec_skill_key")
                )
                if name and name not in completed_skill_names:
                    completed_skill_names.append(name)
            successful_count = sum(
                r.get("skill_success") is True
                for r in completed_batch["results"]
            )
            skill_success = (
                "true" if successful_count == len(completed_batch["results"])
                else ("partial" if successful_count else "false")
            )
            batch_result_payload = {
                "dialogue_id": dialogue_id,
                "completed_batch_key": completed_batch_key,
                "batch_id": completed_batch["batch_id"],
                "status": skill_success,
                "successful_count": successful_count,
                "total_count": len(completed_batch["results"]),
                "results": completed_batch["results"],
            }
            hook_trigger(
                hook_events.COORDINATOR_BATCH_RESULT,
                dict(batch_result_payload),
                ctx=AgentContext(),
            )
            emit_event(
                hook_events.COORDINATOR_BATCH_RESULT,
                dict(batch_result_payload),
            )
            completed_skill_artifacts = merge_dialogue_artifacts(
                databus, collect_batch_artifacts(completed_batch)
            )
            # Accept the aggregate before any later early-return guard (for
            # example max_iterations). C++ final gating waits for this marker;
            # delaying it until message assembly can otherwise deadlock a
            # successfully parsed terminal Batch.
            completed_batch_message = build_skill_batch_completion_message(
                completed_batch
            )
            if databus.is_connected():
                _remember_completed_batch_dispatches(databus, dialogue_id, completed_batch)
                _mark_completed_batch_consumed(databus, completed_batch_key)
        except (ValueError, TypeError) as exc:
            error_text = f"内部错误：并行 Skill 批次结果读取失败（{exc}）"
            emit_event(hook_events.COORDINATOR_LLM_ERROR, {
                "error": str(exc),
                "completed_batch_key": completed_batch_key,
            }, level="ERROR")
            if databus.is_connected():
                try:
                    consume_error_key = f"{completed_batch_key}:consume_error"
                    if not databus.put(consume_error_key, str(exc)):
                        print(
                            f"[flow]   ERROR: mark batch consume error returned false "
                            f"key={consume_error_key}",
                            file=sys.stderr,
                        )
                    if not databus.put("skill_failed", "true"):
                        print(
                            "[flow]   WARN: mark skill_failed returned false",
                            file=sys.stderr,
                        )
                except Exception as marker_exc:
                    print(
                        f"[flow]   WARN: mark batch consume error failed: "
                        f"{marker_exc}",
                        file=sys.stderr,
                    )
            _emit_dialogue_end_once("batch_result_error", error_text)
            print(json.dumps({
                "response": error_text,
                "status": "batch_result_error",
            }, ensure_ascii=False))
            put_e2e_response(databus, error_text)
            print(f"[timing] batch_result_error total={time.time()-_t0:.1f}s",
                  file=sys.stderr)
            return

    if (
        completed_batch is None
        and completed_skill
        and completed_skill != "dummy_pre_skill"
    ):
        completed_skill_artifacts = merge_dialogue_artifacts(
            databus, completed_skill_artifacts
        )
    # Read stream flag from AF_INPUT (bridge sends {"stream": bool, "messages": [...]})
    stream = parse_stream(_input())
    # Persist to DataBus for downstream nodes (skill_executor, etc.)
    if stream:
        databus.put("stream", "true")

    # 读取刚完成的 skill 最近一次 dispatch 的 prompt(F2 双维度重复判定用:
    # 同名 skill + 同 prompt 才算重复;同名不同 prompt = 新任务放行)。
    last_prompts: dict[str, Any] = {}
    completed_names_for_guard = (
        completed_skill_names
        if completed_skill_names
        else ([completed_skill] if completed_skill else [])
    )
    if completed_names_for_guard and databus.is_connected():
        try:
            for name in completed_names_for_guard:
                last_prompts[name] = databus.get_global(
                    f"last_prompt:{dialogue_id}:{name}"
                )
        except Exception as exc:
            print(f"[flow]   WARN: read last_prompt failed: {exc}", file=sys.stderr)
            last_prompts = {}

    if (
        completed_batch is None
        and completed_skill
        and completed_skill != "dummy_pre_skill"
        and databus.is_connected()
    ):
        completed_prompt = last_prompts.get(completed_skill)
        if isinstance(completed_prompt, str):
            _remember_completed_dispatch(
                databus, dialogue_id, completed_skill, completed_prompt,
                terminal_status=skill_success,
            )

    # ── Coordinator iteration tracking ──
    # round remains the compatibility control counter. Separate trace-only
    # counters below distinguish activation, LLM decision, completion and
    # in-activation correction semantics.
    raw_decision_index = (
        databus.get("_coordinator_decision_index")
        if not is_first_trigger and databus.is_connected() else None
    )
    try:
        _decision_index = (
            int(raw_decision_index)
            if raw_decision_index is not None else max(0, current_round - 1)
        )
    except (ValueError, TypeError):
        _decision_index = max(0, current_round - 1)
    if databus.is_connected():
        databus.put("_coordinator_round", str(current_round))
        databus.put("_coordinator_decision_index", str(_decision_index))

    # ── Max iterations guard: break before LLM call ──
    if current_round > max_iterations:
        emit_event(hook_events.COORDINATOR_MAX_ITERATIONS, {"round": current_round, "max_iterations": max_iterations}, level="WARN")
        response_text = llm_error_response(completed_skill_artifacts)
        # ── hooks: 回合级出口（观察，不可干预——固定错误文案）──
        _emit_dialogue_end_once("max_iterations", response_text)
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
    if completed_batch is not None:
        history = attach_skill_completion_result(
            history,
            completed_batch_message,
            batch_id=str(completed_batch.get("batch_id", "")),
            skill_names=completed_skill_names,
        )
        messages.extend(history)
    elif completed_skill and completed_skill != "dummy_pre_skill":
        # Re-trigger: use persisted history + skill completion message
        # collect_skill_results is a 30s databus.get() fallback — skip when env var has the data.
        collected = [] if completed_skill_result else collect_skill_results(databus, [completed_skill])
        history = attach_skill_completion_result(
            history,
            build_skill_completion_message(
                completed_skill,
                skill_success,
                completed_skill_result,
                collected,
            ),
            skill_names=[completed_skill],
        )
        messages.extend(history)
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
            response_text = append_artifacts_markdown(
                "已达协调工具循环上限，基于已有结果输出。",
                completed_skill_artifacts,
            )
            output = {"response": response_text, "status": "tool_loop_max_rounds"}
            _emit_dialogue_end_once("tool_loop_max_rounds", response_text)
            print(json.dumps(output, ensure_ascii=False))
            put_e2e_response(databus, response_text)
            print(f"[timing] tool_loop_max_rounds total={time.time()-_t0:.1f}s", file=sys.stderr)
            return

        _decision_index += 1
        if databus.is_connected():
            databus.put("_coordinator_decision_index", str(_decision_index))
        if _evolution is not None and not _evolution.allow_loop({
            "ref": {"run": dialogue_id, "key": f"coordinator-loop:{_decision_index}"},
            "loop_id": "coordinator", "state": messages[-1] if messages else {},
        }, user_id=_user_id()):
            response_text = append_artifacts_markdown(
                "循环控制已停止重复协调，基于已有结果输出。", completed_skill_artifacts,
            )
            _emit_dialogue_end_once("loop_control_stop", response_text)
            print(json.dumps({"response": response_text, "status": "loop_control_stop"}, ensure_ascii=False))
            put_e2e_response(databus, response_text)
            return
        _trace_indices = {
            "activation_index": current_round,
            "decision_index": _decision_index,
            "skill_completion_index": max(0, current_round - 1),
            "internal_tool_correction_index": _tool_loop_round - 1,
        }
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
                    _emit_dialogue_end_once(
                        "interrupted", response_text, AgentContext(messages=messages)
                    )
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
                "activation_index": str(_trace_indices["activation_index"]),
                "decision_index": str(_trace_indices["decision_index"]),
                "internal_tool_correction_index": str(
                    _trace_indices["internal_tool_correction_index"]
                ),
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
                **_trace_indices,
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
                    round_i=current_round,
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
            _llm_call_id = raw.get("_agentflow_call_id", "")
            hook_trigger(hook_mounts.COORDINATOR_V1_AFTER_LLM_CALL, {
                "round": current_round,
                "call_id": _llm_call_id,
                "raw_choices": raw.get("choices", [{}])[0] if raw.get("choices") else {},
                **_trace_indices,
            }, ctx=_llm_ctx)
            _effective_response = recorder.record_effective_response(_llm_call_id, raw)
            hook_trigger(hook_events.COORDINATOR_LLM_RESULT, {
                "round": current_round,
                "call_id": _llm_call_id,
                "response": _effective_response,
                **_trace_indices,
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
                **_trace_indices,
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
                _emit_dialogue_end_once("llm_error", response_text)
                put_e2e_response(databus, response_text)
                _t6 = time.time()
                print(f"[timing] output={_t6-_t0:.1f}s total", file=sys.stderr)
                print(json.dumps(output, ensure_ascii=False))
                return

        # ── 检测 LLM 返回空业务结果（不依赖 token 计数） ──
        # 复用 coordinator_common.is_empty_llm_response(单一真相源 F4,霰弹式收敛)
        _is_empty_response = is_empty_llm_response(raw)
        if _is_empty_response:
            err_msg = "LLM returned empty response (no content and no tool calls)"
            emit_event(hook_events.COORDINATOR_LLM_ERROR, {
                "round": current_round,
                "error": err_msg,
                "duration_s": round(_t5 - _t0, 1),
            }, level="ERROR")
            response_text = llm_error_response(completed_skill_artifacts)
            output = {"response": response_text}
            _emit_dialogue_end_once("llm_error", response_text)
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
            _emit_dialogue_end_once(
                "normal", response_text, _final_ctx, can_intervene=True
            )
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
            _emit_dialogue_end_once("max_rounds_forced_output", response_text)
            print(json.dumps({"response": response_text}, ensure_ascii=False))
            put_e2e_response(databus, response_text)
            save_session_messages(recorder, messages)
            print(f"[timing] max_rounds_forced_output total={time.time()-_t0:.1f}s", file=sys.stderr)
            return

        # Collect dispatched skill names for progress reporting (ordered set via dict)
        _dispatched_skills: dict[str, None] = {}
        run_skills_call_count = sum(
            1
            for tc in tool_calls
            if tc.get("function", {}).get("name", "") == "run_skills"
        )
        multiple_run_skills_error = (
            "one assistant response must contain exactly one run_skills call; "
            "put every parallel Skill in that call's skills array"
            if run_skills_call_count > 1
            else ""
        )
        for tc in tool_calls:
            func = tc.get("function", {})
            name = func.get("name", "")
            arguments_error = ""
            try:
                args = json.loads(func.get("arguments", "{}"))
            except (json.JSONDecodeError, TypeError) as exc:
                args = {}
                arguments_error = f"arguments must be valid JSON: {exc}"
            if not isinstance(args, dict):
                arguments_error = "arguments must decode to a JSON object"
                args = {}

            if name == "run_skills":
                validation_error = (
                    multiple_run_skills_error
                    or arguments_error
                    or _validate_run_skills_args(args, _AVAILABLE_SKILLS)
                )
                raw_skill_names = args.get("skills", [])
                skill_names = raw_skill_names if isinstance(raw_skill_names, list) else []
                requested_skill_names = [
                    _s.get("name", "") if isinstance(_s, dict) else str(_s)
                    for _s in skill_names
                ]
                requested_skill_names = [name for name in requested_skill_names if name]
                # F2 拦截(CHANGE-20260811-055):连续 round 重复 dispatch 已完成的 skill(无 force)→ 拒绝。
                # F1(提示词)无法完全阻止 LLM 偶发二次 dispatch,此处代码层强制拦截——
                # 避免重复执行整段 skill 浪费耗时(超时现场 140842 同源:二次 dispatch 空 skills/重复执行)。
                force_requested = bool(args.get("force"))
                duplicate_completed_skill = next((
                    name for name in completed_names_for_guard
                    if _is_duplicate_dispatch(
                        name, skill_names, force_requested,
                        last_prompts.get(name),
                    )
                ), "")

                if not validation_error and not duplicate_completed_skill:
                    duplicate_completed_skill = _find_persisted_completed_dispatch(
                        databus, dialogue_id, skill_names, force_requested
                    )
                _dispatch_ctx = AgentContext(messages=messages)
                if validation_error:
                    dispatch_result = _invalid_run_skills_result(validation_error)
                    result_str = json.dumps(dispatch_result, ensure_ascii=False)
                elif duplicate_completed_skill:
                    dispatch_result = {
                        "status": "duplicate_blocked",
                        "skills": [],
                        "exec_skill_keys": [],
                        "rejected": [{
                            "name": duplicate_completed_skill,
                            "stage": "duplicate_guard",
                            "error": "completed name+prompt dispatched again without force",
                        }],
                        "warnings": [],
                        "error": f"skill '{duplicate_completed_skill}' 已用相同 prompt 完成，不得重复 dispatch（除非 force=true）",
                        "response": f"skill '{duplicate_completed_skill}' 已有相同输入的完成结果，请直接使用，不要重复执行。",
                    }
                    result_str = json.dumps(dispatch_result, ensure_ascii=False)
                    emit_event(hook_events.COORDINATOR_DUPLICATE_SKILL_DISPATCH_BLOCKED, {
                        "round": current_round,
                        "skill": duplicate_completed_skill,
                    }, level="WARN")
                else:
                    # ── hooks: skill 分发前挂载点（阻断本轮 dispatch，ctx.blocked 语义）──
                    hook_trigger(
                        hook_mounts.COORDINATOR_BEFORE_SKILL_DISPATCH,
                        {
                            "round": current_round,
                            "skill_names": requested_skill_names,
                        },
                        can_intervene=True,
                        ctx=_dispatch_ctx,
                    )
                    if _dispatch_ctx.blocked:
                        dispatch_result = {
                            "status": "blocked_by_hook",
                            "skills": [],
                            "exec_skill_keys": [],
                            "rejected": [{
                                "name": skill_name,
                                "stage": "before_dispatch_hook",
                                "error": "dispatch blocked by hook",
                            } for skill_name in requested_skill_names],
                            "warnings": [],
                            "skipped_by_hook": True,
                            "response": "本轮 skill 分发已被 hook 阻断，请直接基于已有信息回复用户。",
                        }
                        result_str = json.dumps(dispatch_result, ensure_ascii=False)
                    else:
                        result_str = handle_run_skills_call(args, databus, session_id, dialogue_id)
                        try:
                            dispatch_result = json.loads(result_str)
                        except (json.JSONDecodeError, TypeError):
                            dispatch_result = {}
                accepted_skill_names = (
                    dispatch_result.get("skills", [])
                    if dispatch_result.get("status") in {
                        "dispatched", "partially_dispatched",
                    }
                    else []
                )
                for skill_name in accepted_skill_names:
                    if skill_name:
                        _dispatched_skills[str(skill_name)] = None
                # ── hooks: skill 分发后挂载点（所有 run_skills 结果均进入轨迹）──
                hook_trigger(hook_mounts.COORDINATOR_AFTER_SKILL_DISPATCH, {
                    "round": current_round,
                    "status": dispatch_result.get("status", "dispatch_failed"),
                    "requested_skill_names": requested_skill_names,
                    "skill_names": accepted_skill_names,
                    "batch_id": dispatch_result.get("batch_id", ""),
                    "exec_skill_keys": dispatch_result.get("exec_skill_keys", []),
                    "dispatches": dispatch_result.get("dispatches", []),
                    "rejected": dispatch_result.get("rejected", []),
                    "warnings": dispatch_result.get("warnings", []),
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
                try:
                    if 'databus' in dir() and databus.is_connected():
                        for skill_name_ in _dispatched_skills:
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
            retry_payload = {
                "round": current_round,
                "tool_loop_round": _tool_loop_round,
                "action": "retry",
                "reason": "no_skill_dispatched",
                "tool_calls": tool_calls,
                "elapsed_s": round(time.time() - _t0, 1),
            }
            emit_event(
                hook_events.COORDINATOR_TOOL_LOOP_RETRY,
                dict(retry_payload),
                level="WARN",
            )
            hook_trigger(
                hook_events.COORDINATOR_TOOL_LOOP_RETRY,
                dict(retry_payload),
                ctx=AgentContext(messages=messages),
            )
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


def main():
    """Run the production entrypoint and emit an observation-only error terminal."""
    install_stderr_redaction()
    started_at = time.monotonic()
    try:
        return execute()
    except Exception as exc:
        try:
            load_hooks_from_dir(os.path.join(AGENT_PACKAGE_DIR, "harness", "hooks"))
            hook_trigger(
                hook_events.AGENT_DIALOGUE_ERROR,
                {
                    "session_id": _session_id(),
                    "dialogue_id": _dialogue_id(),
                    "status": "unhandled_exception",
                    "error_type": type(exc).__name__,
                    "error": str(exc),
                    "elapsed_s": round(time.monotonic() - started_at, 1),
                },
                ctx=AgentContext(),
            )
        except Exception as hook_exc:
            # 轨迹钩子失败不能替换 Coordinator 的原始异常。
            print(
                f"[flow]   WARN: dialogue error hook failed: {hook_exc}",
                file=sys.stderr,
            )
        raise


if __name__ == "__main__":
    main()
