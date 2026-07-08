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
import os
import re
import sys
import time
import uuid
from pathlib import Path
from urllib.parse import quote

import yaml

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from databus_client import DataBusClient
from llm_call import (
    call_llm,
    compact_messages,
    estimate_message_tokens,
    get_compaction_threshold,
    get_model_max_tokens,
    get_recorder,
    resolve_llm_config,
)
from observability import emit_event
from parse_af_input import parse_af_input, parse_stream
from prompts import (
    ARTIFACT_DOWNLOAD_TEMPLATE,
    ARTIFACT_HTML_TEMPLATE,
    ARTIFACT_IMAGE_TEMPLATE,
    ARTIFACTS_SECTION_HEADER,
    COORDINATOR_FALLBACK_SYSTEM,
    DECISION_FAILURE_PROMPT,
    DECISION_SUCCESS_PROMPT,
    LLM_ERROR_NO_ARTIFACTS,
    LLM_ERROR_WITH_ARTIFACTS,
    PROTOCOL_TEMPLATE,
    RUN_SKILLS_TOOL_DEFINITION,
    SKILL_COMPLETION_TEMPLATE,
)
from round_reminder import build_round_reminder

NODES_DIR = os.path.dirname(os.path.abspath(__file__))
AGENT_PACKAGE_DIR = os.path.dirname(NODES_DIR)
AGENT_MD_PATH = os.path.join(AGENT_PACKAGE_DIR, "agent.md")

# ── Load .env from package root (business env vars: API_KEY, AF_LLM_*) ──
# Framework has no mechanism for business env; dotenv fills the gap.
_DOTENV_PATH = os.path.join(AGENT_PACKAGE_DIR, ".env")
print(f"[flow] dotenv: path={_DOTENV_PATH} exists={os.path.isfile(_DOTENV_PATH)}", file=sys.stderr)
if os.path.isfile(_DOTENV_PATH):
    try:
        from dotenv import load_dotenv
        load_dotenv(_DOTENV_PATH, override=True)
    except Exception:
        pass


# ── Helper functions (must be defined before module-level code below) ──

RE_ENV_VAR = re.compile(r"\$\{(\w+)\}")


def _expand_env(value):
    if isinstance(value, str):
        return RE_ENV_VAR.sub(lambda m: os.environ.get(m.group(1), m.group(0)), value)
    if isinstance(value, dict):
        return {k: _expand_env(v) for k, v in value.items()}
    if isinstance(value, list):
        return [_expand_env(v) for v in value]
    return value


def parse_agent_md(path: str) -> tuple:
    if not os.path.isfile(path):
        return {}, ""
    with open(path, encoding="utf-8") as f:
        raw = f.read()
    parts = raw.split("---", 2)
    if len(parts) < 3:
        return {}, raw.strip()
    try:
        frontmatter = yaml.safe_load(parts[1]) or {}
        frontmatter = _expand_env(frontmatter)
    except Exception:
        frontmatter = {}
    body = parts[2].strip()
    return frontmatter, body


def get_env(key: str, default: str = "") -> str:
    return os.environ.get(key, default)


# ---------------------------------------------------------------------------
# Framework protocol — auto-injected into coordinator system prompt
# ---------------------------------------------------------------------------

# Build available-skills list from agent.md
_AVAILABLE_SKILLS: list[str] = []
_frontmatter, _ = parse_agent_md(AGENT_MD_PATH)
_raw_skills = _frontmatter.get("skills", [])
if isinstance(_raw_skills, list):
    for _s in _raw_skills:
        _name = _s if isinstance(_s, str) else _s.get("name", "")
        if _name and _name not in _AVAILABLE_SKILLS:
            _AVAILABLE_SKILLS.append(_name)

# Assemble COORDINATOR_PROTOCOL with dynamic skill list
_skills_csv = ", ".join(_AVAILABLE_SKILLS)
COORDINATOR_PROTOCOL = PROTOCOL_TEMPLATE.format(available_skills=_skills_csv)

# Assemble RUN_SKILLS_TOOL with dynamic skill enum
RUN_SKILLS_TOOL = copy.deepcopy(RUN_SKILLS_TOOL_DEFINITION)
RUN_SKILLS_TOOL["function"]["parameters"]["properties"]["skills"]["items"]["properties"]["name"]["enum"] = list(_AVAILABLE_SKILLS)


# ---------------------------------------------------------------------------
# run_skills interception → DataBus → framework next_skills pipeline
# ---------------------------------------------------------------------------

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

        # ② All skills go through 'skill-runner', differentiated by exec_skill_key
        next_skills_specs.append({
            "name": "skill-runner",
            "exec_skill_key": dispatch_key,
            "force": True,
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


_VERSION_SUFFIX = re.compile(r"-(\d+)$")


def _dedup_versioned_artifacts(paths: list[str]) -> list[str]:
    """Keep only the highest-numbered variant when filenames share a ``-N`` stem.

    ``outfit-review-1.png`` and ``outfit-review-2.png`` are treated as versions
    of the same artifact family (base stem ``outfit-review``); only the
    highest-numbered one survives.  Files without a ``-N`` suffix are kept
    unchanged.
    """
    best: dict[str, tuple[int, str]] = {}
    for path in paths:
        stem = Path(path).stem
        match = _VERSION_SUFFIX.search(stem)
        base = stem[: match.start()] if match else stem
        num = int(match.group(1)) if match else 0
        prev = best.get(base)
        if prev is None or num > prev[0]:
            best[base] = (num, path)
    return [path for _, path in best.values()]


def append_artifacts_markdown(content: str, artifacts_json: str) -> str:
    """Append browser-accessible Markdown for generated artifacts (images, HTML, downloads)."""
    try:
        artifacts = json.loads(artifacts_json) if artifacts_json else []
    except (json.JSONDecodeError, TypeError):
        artifacts = []
    items = [item for item in artifacts if isinstance(item, str)]
    legacy_absolute_images = re.findall(
        r"!\[[^\]]*\]\((/[^)]+\.(?:png|jpe?g|webp|gif))\)", content, re.I
    )
    content = rewrite_local_artifact_links(
        content, list(dict.fromkeys(items + legacy_absolute_images))
    )
    if not items:
        return content

    _image_suffixes = {".png", ".jpg", ".jpeg", ".webp", ".gif"}
    images = _dedup_versioned_artifacts(
        [p for p in items if Path(p).suffix.lower() in _image_suffixes]
    )
    html_reports = [p for p in items if p.lower().endswith(".html")]
    downloads = [p for p in items if p not in images and p not in html_reports]

    blocks = []
    if images:
        for index, path in enumerate(images, 1):
            url = path if path.startswith(("http://", "https://")) else local_artifact_url(path)
            if url not in content:
                blocks.append(ARTIFACT_IMAGE_TEMPLATE.format(index=index, url=url))
    if html_reports:
        for index, path in enumerate(html_reports, 1):
            url = path if path.startswith(("http://", "https://")) else local_artifact_url(path)
            if url not in content:
                blocks.append(ARTIFACT_HTML_TEMPLATE.format(index=index, url=url))
    if downloads:
        for index, path in enumerate(downloads, 1):
            url = path if path.startswith(("http://", "https://")) else local_artifact_url(path)
            if url not in content:
                blocks.append(ARTIFACT_DOWNLOAD_TEMPLATE.format(index=index, url=url))

    if not blocks:
        return content
    return content.rstrip() + "\n\n" + ARTIFACTS_SECTION_HEADER + "\n\n" + "\n\n".join(blocks)


def llm_error_response(artifacts_json: str) -> str:
    """Build an honest fallback based on artifacts actually reported by a skill."""
    try:
        artifacts = json.loads(artifacts_json) if artifacts_json else []
    except (json.JSONDecodeError, TypeError):
        artifacts = []
    has_artifacts = any(isinstance(item, str) and item for item in artifacts)
    if has_artifacts:
        return append_artifacts_markdown(LLM_ERROR_WITH_ARTIFACTS, artifacts_json)
    return LLM_ERROR_NO_ARTIFACTS


def rewrite_local_artifact_links(content: str, artifacts: list[str]) -> str:
    """Rewrite local Markdown and HTML references using collected artifacts.

    Skill output remains untouched at its source.  Only references that resolve
    to a file already admitted to the current artifact list are publicized.
    """
    local_artifacts = {
        os.path.realpath(path): path
        for path in artifacts
        if not path.startswith(("http://", "https://")) and os.path.isfile(path)
    }

    def resolve_url(reference: str) -> str | None:
        reference = reference.strip()
        if not reference or reference.startswith(("http://", "https://", "data:", "#")):
            return None
        candidate = reference if os.path.isabs(reference) else os.path.join(
            AGENT_PACKAGE_DIR, reference
        )
        real_candidate = os.path.realpath(candidate)
        if real_candidate not in local_artifacts:
            return None
        return local_artifact_url(local_artifacts[real_candidate])

    markdown_pattern = re.compile(r"(!?\[[^\]]*\]\()([^)\s]+)(\))", re.I)

    def replace_markdown(match: re.Match) -> str:
        url = resolve_url(match.group(2))
        if not url:
            return match.group(0)
        return f"{match.group(1)}{url}{match.group(3)}"

    content = markdown_pattern.sub(replace_markdown, content)
    html_pattern = re.compile(
        r"(?P<prefix>\b(?:src|href|poster)\s*=\s*)(?P<quote>[\"'])(?P<path>[^\"']+)(?P=quote)",
        re.I,
    )

    def replace_html(match: re.Match) -> str:
        url = resolve_url(match.group("path"))
        if not url:
            return match.group(0)
        quote_char = match.group("quote")
        return f"{match.group('prefix')}{quote_char}{url}{quote_char}"

    return html_pattern.sub(replace_html, content)


def rewrite_local_image_links(content: str) -> str:
    """Backward-compatible absolute Markdown image rewriting helper."""
    absolute_images = re.findall(
        r"!\[[^\]]*\]\((/[^)]+\.(?:png|jpe?g|webp|gif))\)", content, re.I
    )
    return rewrite_local_artifact_links(content, absolute_images)


def local_artifact_url(path: str) -> str:
    """Map a package-local path to a hierarchy-preserving HTTP URL."""
    packages_root = os.path.dirname(AGENT_PACKAGE_DIR)
    relative = os.path.relpath(os.path.realpath(path), packages_root)
    if relative.startswith(".."):
        return f"/api/artifacts/outfit-assistant?path={quote(path, safe='')}"
    return f"/api/artifacts/outfit-assistant/files/{quote(relative, safe='/')}"


# ---------------------------------------------------------------------------
# Main — single LLM decision per trigger
# ---------------------------------------------------------------------------

def execute():
    _t0 = time.time()
    raw_input = get_env("AF_INPUT", "")
    dialogue_id = get_env("AF_DIALOGUE_ID", "")
    session_id = get_env("AF_SESSION_ID", "")

    # Framework-injected skill completion info (from node inputs → AF_ env vars)
    completed_skill = get_env("AF_COMPLETED_SKILL_NAME", "")
    skill_success = get_env("AF_SKILL_SUCCESS", "")
    completed_skill_result = get_env("AF_SKILL_RESULT", "")
    completed_skill_artifacts = get_env("AF_SKILL_ARTIFACTS", "")

    max_iterations = int(get_env("AF_MAX_COORDINATOR_ITERATIONS", "10"))
    max_completion_tokens = int(get_env("AF_MAX_COMPLETION_TOKENS", "8192"))

    frontmatter, agent_md_body = parse_agent_md(AGENT_MD_PATH)
    model_cfg = frontmatter.get("model", {})
    # model can be a plain string (e.g. "MiniMax-M3") or a dict {name: "..."}
    agent_md_model = model_cfg if isinstance(model_cfg, str) else model_cfg.get("name", "")

    # 使用 fallback 链解析最终 LLM 配置
    llm_cfg = resolve_llm_config(agent_md_model)
    model_name = llm_cfg["model"]
    api_key = llm_cfg["api_key"]
    base_url = llm_cfg["base_url"]

    # ── flow debug ──
    emit_event("coordinator.trigger", {
        "trigger_type": "skill_completion" if completed_skill else "first",
        "completed_skill": completed_skill,
        "skill_success": skill_success,
        "model": model_name,
        "max_iterations": max_iterations,
    })
    print(f"[flow] coordinator_decide triggered: session={session_id} dialogue={dialogue_id} model={model_name}", file=sys.stderr)
    _t1 = time.time()
    if completed_skill:
        print(f"[flow]   trigger_type=skill_completion completed_skill={completed_skill} success={skill_success}", file=sys.stderr)
    else:
        print(f"[flow]   trigger_type=first", file=sys.stderr)

    databus = DataBusClient()
    recorder = get_recorder()
    # Read stream flag from AF_INPUT (bridge sends {"stream": bool, "messages": [...]})
    stream = parse_stream(get_env("AF_INPUT", ""))
    # Persist to DataBus for downstream nodes (skill_executor, etc.)
    if stream:
        databus.put("stream", "true")

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
    print(f"[flow]   coordinator_round={current_round}/{max_iterations} "
          f"(first_trigger={is_first_trigger})", file=sys.stderr)

    # ── Max iterations guard: break before LLM call ──
    if current_round > max_iterations:
        emit_event("coordinator.max_iterations", {"round": current_round, "max_iterations": max_iterations}, level="WARN")
        print(f"[flow]   ⚠️  max iterations ({max_iterations}) reached, force ending", file=sys.stderr)
        response_text = llm_error_response(completed_skill_artifacts)
        output = {"response": response_text, "status": "max_iterations_reached"}
        print(json.dumps(output, ensure_ascii=False))
        if databus.is_connected():
            try:
                databus._client.raw().put("_e2e_response", response_text)
            except Exception:
                pass
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
    af_messages: list[dict] = []
    try:
        parsed = json.loads(raw_input)
        if isinstance(parsed, dict) and "messages" in parsed and isinstance(parsed["messages"], list):
            af_messages = parsed["messages"]
    except (json.JSONDecodeError, TypeError):
        pass

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

    # ── Token budget: compact messages if exceeding threshold ──
    model_max_tokens = get_model_max_tokens()
    compaction_threshold = get_compaction_threshold()
    before_tokens = estimate_message_tokens(messages)
    compacted = compact_messages(messages, model_max_tokens, compaction_threshold)
    after_tokens = estimate_message_tokens(compacted)
    if compacted is not messages:
        print(f"[flow]   compaction: {before_tokens}→{after_tokens} tok "
              f"(threshold={int(model_max_tokens * compaction_threshold)}), "
              f"dropped {len(messages) - len(compacted)} msgs", file=sys.stderr)
        messages = compacted

    # ── Per-round budget reminder (delegated to shared module) ──
    reminder = build_round_reminder(
        round_i=current_round - 1,  # 0-based for shared module
        max_rounds=max_iterations,
        elapsed=time.time() - _t0,
        node_timeout_sec=int(get_env("AF_NODE_TIMEOUT_SEC", "180")),
        custom_hint="本轮必须输出，可以是任务结论或当前执行结果",
    )
    if reminder:
        messages.append(reminder)

    _t4 = time.time()
    print(f"[timing] llm_call_start={_t4-_t0:.1f}s", file=sys.stderr)
    # ── Progress: coordinator is calling LLM (may take >60s) ──
    try:
        if 'databus' in dir():
            databus.response_push("coordinator 决策中…", type="progress",
                                  skill="coordinator", status="running",
                                  source="coordinator")
    except Exception as exc:
        print(f"[flow]   WARN: coordinator progress push failed: {exc}", file=sys.stderr)
    emit_event("coordinator.llm_call_start", {
        "round": current_round,
        "messages": len(messages),
        "model": model_name,
        "tools": True,
        "stream": stream,
    })
    raw = call_llm(
        messages, tools=[RUN_SKILLS_TOOL],
        model=model_name,
        base_url=base_url,
        api_key=api_key,
        max_completion_tokens=max_completion_tokens,
        stream=stream,
        source="coordinator")
    _t5 = time.time()
    print(f"[timing] llm_call_end={_t5-_t0:.1f}s (llm={_t5-_t4:.1f}s)", file=sys.stderr)

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
    print(
        f"[obs] coord_decide: finish={_obs_finish} "
        f"tool_calls={_obs_tc_count} names={_obs_tc_names or 'none'} "
        f"content_preview={_obs_content_preview}",
        file=sys.stderr,
    )
    emit_event("coordinator.llm_result", {
        "round": current_round,
        "finish_reason": _obs_finish,
        "tool_call_count": _obs_tc_count,
        "tool_call_names": _obs_tc_names,
        "llm_duration_s": round(_t5 - _t4, 1),
        "total_duration_s": round(_t5 - _t0, 1),
    })

    if raw.get("_llm_error"):
        err_msg = raw.get("error", "LLM call failed")
        emit_event("coordinator.llm_error", {
            "round": current_round,
            "error": err_msg,
            "duration_s": round(_t5 - _t0, 1),
        }, level="ERROR")
        print(
            f"[flow]   ⚠️  LLM error after call_llm internal retries, returning fallback: {err_msg}",
            file=sys.stderr,
        )
        # Return fallback response instead of crashing — user gets something,
        # workflow completes, coordinator can be re-triggered.
        response_text = llm_error_response(completed_skill_artifacts)
        output = {"response": response_text}
        if databus.is_connected():
            try:
                databus._client.raw().put("_e2e_response", response_text)
            except Exception:
                pass
        _t6 = time.time()
        print(f"[timing] output={_t6-_t0:.1f}s total", file=sys.stderr)
        print(json.dumps(output, ensure_ascii=False))
        return

    choice = raw.get("choices", [{}])[0]
    msg = choice.get("message", {})
    tool_calls = msg.get("tool_calls")
    messages.append(msg)

    if tool_calls:
        # ── Max iterations guard: if this is the last allowed round, force final ──
        #     output instead of dispatching more skills.
        if current_round >= max_iterations:
            print(f"[flow]   ⚠️  max iterations ({max_iterations}) reached, forcing final output "
                  f"even though LLM requested tools", file=sys.stderr)
            raw_response = msg.get("content", "")
            import re as _re
            think_match = _re.search(r"</think>\s*", raw_response)
            if think_match:
                response_text = raw_response[think_match.end():].strip()
            else:
                response_text = raw_response or "已达协调轮次上限，基于已有结果输出。"
            response_text = append_artifacts_markdown(response_text, completed_skill_artifacts)
            emit_event("coordinator.max_rounds_forced_output", {
                "round": current_round,
                "max_iterations": max_iterations,
                "response_length": len(response_text),
            }, level="WARN")
            print(json.dumps({"response": response_text}, ensure_ascii=False))
            if databus.is_connected():
                try:
                    databus._client.raw().put("_e2e_response", response_text)
                except Exception:
                    pass
            messages_to_save = messages[1:]
            if recorder.is_connected:
                recorder.write_session_messages(messages_to_save)
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
                # Collect skill names for progress reporting
                for _s in skill_names:
                    _sn = _s.get("name", "") if isinstance(_s, dict) else str(_s)
                    if _sn:
                        _dispatched_skills[_sn] = None
                # Log full per-skill parameters
                for i, s in enumerate(skill_names, 1):
                    p = s.get("prompt", "")[:100]
                    ls = s.get("load_skills", [])
                    print(f"[flow]   skill[{i}]: name={s.get('name','?')} prompt='{p}' load_skills={ls}", file=sys.stderr)
                print(f"[flow]   LLM decision: run_skills reason={args.get('reason','')[:80]}", file=sys.stderr)
                result_str = handle_run_skills_call(args, databus, session_id, dialogue_id)
                # Log dispatch result
                result_json = json.loads(result_str)
                print(f"[flow]   dispatch result: status={result_json.get('status')} skills={result_json.get('skills')}", file=sys.stderr)
            else:
                print(f"[flow]   LLM decision: unknown_tool={name}", file=sys.stderr)
                result_str = json.dumps({"error": f"Unknown tool: {name}"})

            messages.append({
                "role": "tool",
                "tool_call_id": tc.get("id", name),
                "content": result_str,
            })

        # Persist conversation history (coordinator yields control, waits for re-trigger)
        messages_to_save = messages[1:]
        if recorder.is_connected:
            recorder.write_session_messages(messages_to_save)

        # Output coordinator_status + progress response for frontend
        _t6 = time.time()
        print(f"[timing] skill_dispatched={_t6-_t0:.1f}s", file=sys.stderr)
        if _dispatched_skills:
            skill_labels = "、".join(_dispatched_skills)
            emit_event("coordinator.skills_dispatched", {
                "round": current_round,
                "skill_names": skill_labels,
                "elapsed_s": round(_t6 - _t0, 1),
            })

            # Extract LLM's intermediate reasoning from this round's response
            llm_before_call = msg.get("content", "")
            llm_summary = ""
            if llm_before_call:
                # Strip <think> tags (same logic as final response path)
                think_match = re.search(r"</think>\s*", llm_before_call)
                if think_match:
                    llm_before_call = llm_before_call[think_match.end():].strip()
                else:
                    llm_before_call = llm_before_call.strip()
                # Keep concise — frontend progress area shows single-line text
                if len(llm_before_call) > 200:
                    llm_before_call = llm_before_call[:200] + "…"
                if llm_before_call:
                            llm_summary = f"\n分析：{llm_before_call}"

                skill_progress = f"正在执行 {skill_labels}{llm_summary}"
                try:
                    if 'databus' in dir() and databus.is_connected():
                        for skill_name_ in skill_labels.strip().split():
                            sn = skill_name_.strip()
                            if sn:
                                databus.response_push(sn, type="progress", skill=sn, status="running",
                                                      source="coordinator")
                except Exception as exc:
                    print(f"[flow]   WARN: dispatch progress push failed: {exc}", file=sys.stderr)
            # Clean up "response" key to prevent stale progress message
            # from leaking into the next coordinator round.
            if databus.is_connected():
                databus.delete("response")
            emit_event("coordinator.dispatch_loop_end", {
                "round": current_round,
                "skills_dispatched": skill_labels,
                "elapsed_s": round(_t6 - _t0, 1),
            })
        else:
            # All tool_calls were unknown — no skills dispatched.
            print(json.dumps({
                "response": "工具调用失败，请重试。",
            }, ensure_ascii=False))
    else:
        # LLM made no tool_call → final response
        print(f"[flow]   LLM decision: final_response (no tool_call)", file=sys.stderr)
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
        emit_event("coordinator.final_response", {
            "round": current_round,
            "has_tool_call": False,
            "response_length": len(response_text),
            "elapsed_s": round(time.time() - _t0, 1),
        })
        print(json.dumps({"response": response_text}, ensure_ascii=False))
        _t6 = time.time()
        print(f"[timing] final_output={_t6-_t0:.1f}s total", file=sys.stderr)

        # Write to persistent DataBus key for e2e tests (bypasses RESPONSE_POLLER
        # which immediately removes the "response" key). Raw client writes to
        # global namespace, not dialogue-scoped, so any session can read it.
        if databus.is_connected():
            try:
                _t7 = time.time()
                databus._client.raw().put("_e2e_response", response_text)
                print(f"[timing] e2e_put={time.time()-_t7:.1f}s", file=sys.stderr)
            except Exception:
                pass

        messages_to_save = messages[1:]
        if recorder.is_connected:
            _t8 = time.time()
            recorder.write_session_messages(messages_to_save)
            print(f"[timing] session_save={time.time()-_t8:.1f}s", file=sys.stderr)

        _t9 = time.time()
        print(f"[timing] script_exit={_t9-_t0:.1f}s", file=sys.stderr)


if __name__ == "__main__":
    execute()
