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

import json
import os
import re
import sys
import time
import uuid
from urllib.parse import quote

import yaml

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from databus_client import DataBusClient
from llm_call import call_llm, get_recorder

NODES_DIR = os.path.dirname(os.path.abspath(__file__))
AGENT_PACKAGE_DIR = os.path.dirname(NODES_DIR)
AGENT_MD_PATH = os.path.join(AGENT_PACKAGE_DIR, "agent.md")

# ── Load .env from package root (business env vars: API_KEY, AF_LLM_*) ──
# Framework has no mechanism for business env; dotenv fills the gap.
_DOTENV_PATH = os.path.join(AGENT_PACKAGE_DIR, ".env")
if os.path.isfile(_DOTENV_PATH):
    try:
        from dotenv import load_dotenv
        load_dotenv(_DOTENV_PATH)
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

# ── Build available-skills list from agent.md ──
_AVAILABLE_SKILLS: list[str] = []
_frontmatter, _ = parse_agent_md(AGENT_MD_PATH)
_raw_skills = _frontmatter.get("skills", [])
if isinstance(_raw_skills, list):
    for _s in _raw_skills:
        _name = _s if isinstance(_s, str) else _s.get("name", "")
        if _name and _name not in _AVAILABLE_SKILLS:
            _AVAILABLE_SKILLS.append(_name)

COORDINATOR_PROTOCOL = """## 你的工作方式

你是一个任务编排 AI，通过 run_skills 工具委派 skill 执行子任务。
你不能直接读取文件、执行命令或搜索——所有具体工作必须委派给 skill。

## run_skills 使用方式

调用 run_skills 时，为每个 skill 指定：
- name (必填): skill 名称，可用选项：""" + ", ".join(_AVAILABLE_SKILLS) + """
- prompt (必填): 给这个 skill 的任务指令，告诉它应该做什么
- load_skills (可选): skill 应加载的技能文档名称列表
- exec_skill_key (可选): 结构化输入数据的 DataBus key
- force (可选, 默认 false): 强制重新执行已完成的 skill

## 决策规则

- 需要执行子任务 → 调用 run_skills 委派
- 所有子任务完成且结果满意 → 直接回复用户（不调用工具）
- 结果不完整或有新需求 → 再次调用 run_skills 补充
- 可以在一次 run_skills 中委派多个独立子任务（并行执行）
- 每次只做一轮决策，等待 skill 结果后再做下一轮

## 输出格式

当你决定直接回复用户时（finish=stop），必须使用以下格式：

<think>
你的推理过程（评估 skill 结果、判断依据、决策原因等）
</think>
给用户的最终回复内容（markdown 格式）

注意：
- <think> 标签内的内容不会展示给用户，仅用于 trace 记录
- <think> 标签外的内容才是用户看到的最终回复
- 不要在最终回复中重复推理过程的内容
"""

# ---------------------------------------------------------------------------
# run_skills tool definition
# ---------------------------------------------------------------------------

RUN_SKILLS_TOOL = {
    "type": "function",
    "function": {
        "name": "run_skills",
        "description": (
            "委派一个或多个 skill 去执行子任务。每个 skill 会收到你指定的任务指令，"
            "独立完成工作后返回结果。所有 skill 并行执行，全部完成后统一返回。"
            "你可以根据结果决定是否需要再委派新的 skill，或直接回复用户。"
        ),
        "parameters": {
            "type": "object",
            "properties": {
                "skills": {
                    "type": "array",
                    "items": {
                        "type": "object",
                        "properties": {
                            "name": {
                                "type": "string",
                                "enum": _AVAILABLE_SKILLS,
                                "description": "Skill 名称，要从可用列表中选择"
                            },
                            "prompt": {
                                "type": "string",
                                "description": "给这个 skill 的任务指令。"
                                             "告诉 skill executor 应该完成什么具体工作。"
                            },
                            "load_skills": {
                                "type": "array",
                                "items": {"type": "string"},
                                "description": "该 skill 应加载的额外技能文档名称列表。"
                            },
                            "exec_skill_key": {
                                "type": "string",
                                "description": "DataBus key，指向该 skill 的结构化输入数据。"
                            },
                            "force": {
                                "type": "boolean",
                                "description": "强制重新执行，即使该 skill 已启动过。默认 false。"
                            }
                        },
                        "required": ["name", "prompt"]
                    },
                    "description": "要执行的 skill 列表，每个 skill 带有任务指派信息"
                },
                "reason": {
                    "type": "string",
                    "description": "为什么需要这些 skill（用于审计和可追溯性）"
                }
            },
            "required": ["skills"]
        }
    }
}


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
        "请根据失败原因判断：直接回复用户并说明失败，或在尝试次数允许时重新委派 skill。"
        if str(skill_success).lower() != "true"
        else "请评估结果，决定是否需要再委派 skill 或直接回复用户。"
    )
    return (
        f"Skill '{completed_skill}' 已完成 (success={skill_success})。\n"
        f"结果: {json.dumps(skill_results, ensure_ascii=False, indent=2)}\n"
        f"{decision}"
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


def append_artifacts_markdown(content: str, artifacts_json: str) -> str:
    """Append browser-accessible Markdown for generated image artifacts."""
    try:
        artifacts = json.loads(artifacts_json) if artifacts_json else []
    except (json.JSONDecodeError, TypeError):
        artifacts = []
    images = [item for item in artifacts if isinstance(item, str)]
    content = rewrite_local_image_links(content)
    if not images:
        return content
    blocks = []
    for index, path in enumerate(images, 1):
        url = path if path.startswith(("http://", "https://")) else local_artifact_url(path)
        blocks.append(f"![生成图片 {index}]({url})")
    return content.rstrip() + "\n\n## 生成图片\n\n" + "\n\n".join(blocks)


def llm_error_response(artifacts_json: str) -> str:
    """Build an honest fallback based on artifacts actually reported by a skill."""
    try:
        artifacts = json.loads(artifacts_json) if artifacts_json else []
    except (json.JSONDecodeError, TypeError):
        artifacts = []
    has_artifacts = any(isinstance(item, str) and item for item in artifacts)
    if has_artifacts:
        return append_artifacts_markdown(
            "文本整理暂时失败，已生成的产物仍可查看。",
            artifacts_json,
        )
    return "抱歉，本次模型调用失败，任务未能完成，请稍后重试。"


def rewrite_local_image_links(content: str) -> str:
    """Rewrite absolute local Markdown image paths to the artifact endpoint."""
    pattern = re.compile(r"(!\[[^\]]*\]\()(/[^)]+\.(?:png|jpe?g|webp|gif))(\))", re.I)

    def replace(match: re.Match) -> str:
        path = match.group(2)
        if not os.path.isfile(path):
            return match.group(0)
        url = local_artifact_url(path)
        return f"{match.group(1)}{url}{match.group(3)}"

    return pattern.sub(replace, content)


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
    user_input = get_env("AF_INPUT", "")
    dialogue_id = get_env("AF_DIALOGUE_ID", "")
    session_id = get_env("AF_SESSION_ID", "")

    # Framework-injected skill completion info (from node inputs → AF_ env vars)
    completed_skill = get_env("AF_COMPLETED_SKILL_NAME", "")
    skill_success = get_env("AF_SKILL_SUCCESS", "")
    completed_skill_result = get_env("AF_SKILL_RESULT", "")
    completed_skill_artifacts = get_env("AF_SKILL_ARTIFACTS", "")

    max_completion_tokens = int(get_env("AF_MAX_COMPLETION_TOKENS", "8192"))

    frontmatter, agent_md_body = parse_agent_md(AGENT_MD_PATH)
    model_cfg = frontmatter.get("model", {})
    # model can be a plain string (e.g. "MiniMax-M3") or a dict {name: "..."}
    model_name = model_cfg if isinstance(model_cfg, str) else model_cfg.get("name", "gpt-4o-mini")

    # API key and base URL come from env vars (not agent.md frontmatter).
    # Priority: AF_LLM_API_KEY > API_KEY, AF_LLM_BASE_URL > default.
    api_key = get_env("AF_LLM_API_KEY") or get_env("API_KEY", "")
    if not api_key or "${" in api_key:
        print(json.dumps({"error": "API key not resolved"}, ensure_ascii=False))
        sys.exit(1)
    base_url = get_env("AF_LLM_BASE_URL", "")

    # ── flow debug ──
    print(f"[flow] coordinator_decide triggered: session={session_id} dialogue={dialogue_id} model={model_name}", file=sys.stderr)
    _t1 = time.time()
    if completed_skill:
        print(f"[flow]   trigger_type=skill_completion completed_skill={completed_skill} success={skill_success}", file=sys.stderr)
    else:
        print(f"[flow]   trigger_type=first user_input={user_input[:80]}", file=sys.stderr)

    databus = DataBusClient()
    recorder = get_recorder()
    _t2 = time.time()
    print(f"[timing] init={_t2-_t1:.1f}s", file=sys.stderr)

    # Restore cross-trigger conversation history (llm namespace, session-persistent)
    history = recorder.read_session_messages() if recorder.is_connected else []

    # Assemble system prompt = framework protocol + business intent
    system_prompt = COORDINATOR_PROTOCOL + "\n\n---\n\n" + (agent_md_body or
                     "你是一个任务编排 AI。分析用户需求，委派 skill 执行子任务。")

    messages = [{"role": "system", "content": system_prompt}]
    messages.extend(history)
    _t3 = time.time()
    print(f"[timing] messages_ready={_t3-_t0:.1f}s", file=sys.stderr)

    # Construct this trigger's input
    # dummy_pre_skill is a passthrough hook — treat as first trigger (no result to evaluate)
    if completed_skill and completed_skill != "dummy_pre_skill":
        # Re-trigger: skill completion notification
        messages.append({"role": "user", "content": build_skill_completion_message(
            completed_skill,
            skill_success,
            completed_skill_result,
            collect_skill_results(databus, [completed_skill]),
        )})
    else:
        # First trigger (or dummy_pre_skill hook): user's original request
        messages.append({"role": "user", "content": user_input})

    # Single LLM decision (tools: [run_skills])
    # 4000 tokens to avoid finish=length truncation on long responses.
    #
    # Transport-layer retry (SSL/DNS/timeout) lives INSIDE call_llm — see
    # llm_call.py:_is_transient_curl_error. Coordinator does NOT duplicate
    # network retry here; it only handles _llm_error as a final fallback
    # (after call_llm has exhausted its own retries).
    _t4 = time.time()
    print(f"[timing] llm_call_start={_t4-_t0:.1f}s", file=sys.stderr)
    raw = call_llm(
        messages, tools=[RUN_SKILLS_TOOL],
        model=model_name,
        base_url=base_url,
        api_key=api_key,
        max_completion_tokens=4000)
    _t5 = time.time()
    print(f"[timing] llm_call_end={_t5-_t0:.1f}s (llm={_t5-_t4:.1f}s)", file=sys.stderr)

    if raw.get("_llm_error"):
        err_msg = raw.get("error", "LLM call failed")
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
        for tc in tool_calls:
            func = tc.get("function", {})
            name = func.get("name", "")
            try:
                args = json.loads(func.get("arguments", "{}"))
            except json.JSONDecodeError:
                args = {}

            if name == "run_skills":
                skill_names = args.get("skills", [])
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

        # Output coordinator_status (don't write "response" — avoid overwriting skill results)
        _t6 = time.time()
        print(f"[timing] skill_dispatched={_t6-_t0:.1f}s", file=sys.stderr)
        print(json.dumps({"coordinator_status": "skills_dispatched"},
                         ensure_ascii=False))
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
