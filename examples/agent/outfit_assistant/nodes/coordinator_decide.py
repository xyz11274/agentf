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

# ---------------------------------------------------------------------------
# Framework protocol — auto-injected into coordinator system prompt
# ---------------------------------------------------------------------------

COORDINATOR_PROTOCOL = """## 你的工作方式

你是一个任务编排 AI，通过 run_skills 工具委派 skill 执行子任务。
你不能直接读取文件、执行命令或搜索——所有具体工作必须委派给 skill。

## run_skills 使用方式

调用 run_skills 时，为每个 skill 指定：
- name (必填): skill 名称，对应 harness/skills/ 下的目录
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
                                "description": "Skill 名称，对应 harness/skills/ 下的目录名"
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
# agent.md parsing
# ---------------------------------------------------------------------------

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


# ---------------------------------------------------------------------------
# LLM interaction
# ---------------------------------------------------------------------------

def get_env(key: str, default: str = "") -> str:
    return os.environ.get(key, default)


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
    for i, spec in enumerate(skills, 1):
        dispatch_key = f"dispatch:{i}"
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


# ---------------------------------------------------------------------------
# Main — single LLM decision per trigger
# ---------------------------------------------------------------------------

def execute():
    user_input = get_env("AF_INPUT", "")
    dialogue_id = get_env("AF_DIALOGUE_ID", "")
    session_id = get_env("AF_SESSION_ID", "")

    # Framework-injected skill completion info (from node inputs → AF_ env vars)
    completed_skill = get_env("AF_COMPLETED_SKILL_NAME", "")
    skill_success = get_env("AF_SKILL_SUCCESS", "")

    frontmatter, agent_md_body = parse_agent_md(AGENT_MD_PATH)
    model_cfg = frontmatter.get("model", {})
    # model can be a plain string (e.g. "Minimax-M3") or a dict {name: "..."}
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
    if completed_skill:
        print(f"[flow]   trigger_type=skill_completion completed_skill={completed_skill} success={skill_success}", file=sys.stderr)
    else:
        print(f"[flow]   trigger_type=first user_input={user_input[:80]}", file=sys.stderr)

    databus = DataBusClient()
    recorder = get_recorder()

    # Restore cross-trigger conversation history (llm namespace, session-persistent)
    history = recorder.read_session_messages() if recorder.is_connected else []

    # Assemble system prompt = framework protocol + business intent
    system_prompt = COORDINATOR_PROTOCOL + "\n\n---\n\n" + (agent_md_body or
                     "你是一个任务编排 AI。分析用户需求，委派 skill 执行子任务。")

    messages = [{"role": "system", "content": system_prompt}]
    messages.extend(history)

    # Construct this trigger's input
    # dummy_pre_skill is a passthrough hook — treat as first trigger (no result to evaluate)
    if completed_skill and completed_skill != "dummy_pre_skill":
        # Re-trigger: skill completion notification
        skill_results = collect_skill_results(databus, [completed_skill])
        messages.append({"role": "user", "content":
            f"Skill '{completed_skill}' 已完成 (success={skill_success})。\n"
            f"结果: {json.dumps(skill_results, ensure_ascii=False, indent=2)}\n"
            f"请评估结果，决定是否需要再委派 skill 或直接回复用户。"})
    else:
        # First trigger (or dummy_pre_skill hook): user's original request
        messages.append({"role": "user", "content": user_input})

    # Single LLM decision (tools: [run_skills])
    # 4000 tokens to avoid finish=length truncation on long responses
    raw = call_llm(
        messages, tools=[RUN_SKILLS_TOOL],
        model=model_name,
        base_url=base_url,
        api_key=api_key,
        max_completion_tokens=4000)

    if raw.get("_llm_error"):
        print(json.dumps({"error": raw.get("error", "LLM call failed")},
                          ensure_ascii=False))
        sys.exit(1)

    choice = raw.get("choices", [{}])[0]
    msg = choice.get("message", {})
    tool_calls = msg.get("tool_calls")

    if tool_calls:
        messages.append(msg)
        for tc in tool_calls:
            func = tc.get("function", {})
            name = func.get("name", "")
            try:
                args = json.loads(func.get("arguments", "{}"))
            except json.JSONDecodeError:
                args = {}

            if name == "run_skills":
                skill_names = args.get("skills", [])
                print(f"[flow]   LLM decision: run_skills skills={[s.get('name','?') for s in skill_names]} reason={args.get('reason','')[:60]}", file=sys.stderr)
                result_str = handle_run_skills_call(args, databus, session_id, dialogue_id)
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
        print(json.dumps({"response": response_text}, ensure_ascii=False))

        # Write to persistent DataBus key for e2e tests (bypasses RESPONSE_POLLER
        # which immediately removes the "response" key). Raw client writes to
        # global namespace, not dialogue-scoped, so any session can read it.
        if databus.is_connected():
            try:
                databus._client.raw().put("_e2e_response", response_text)
            except Exception:
                pass

        messages_to_save = messages[1:]
        if recorder.is_connected:
            recorder.write_session_messages(messages_to_save)


if __name__ == "__main__":
    execute()
