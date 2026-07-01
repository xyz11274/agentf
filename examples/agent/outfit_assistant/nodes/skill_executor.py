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
import os
import importlib as _importlib
import importlib.util as _importlib_util
import re
import sys
import yaml

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from databus_client import DataBusClient
from llm_call import call_llm

NODES_DIR = os.path.dirname(os.path.abspath(__file__))
AGENT_PACKAGE_DIR = os.path.dirname(NODES_DIR)
AGENT_MD_PATH = os.path.join(AGENT_PACKAGE_DIR, "agent.md")

# ── Load .env from package root (Plan B: bypass framework for business env vars) ──
# Framework has no mechanism to declare business env (QWEATHER_API_KEY etc.),
# so we load .env here for all node processes that run this module.
# See design doc: agent package env → framework injection is the correct long-term fix.
_DOTENV_PATH = os.path.join(AGENT_PACKAGE_DIR, ".env")
if os.path.isfile(_DOTENV_PATH):
    try:
        from dotenv import load_dotenv
        load_dotenv(_DOTENV_PATH)
    except Exception:
        pass  # dotenv not installed; fall through silently
SKILLS_BASE = os.path.join(AGENT_PACKAGE_DIR, "harness", "skills")

# Skill-scoped tool loading: each skill's scripts/ dir provides its own tools.
# A global harness/tools/ dir is no longer used for skill tools — tools live
# co-located with the skill that owns them (single responsibility + packaging).

class _LocalToolRegistry:
    """Loads domain tools from a specific skill's scripts/ directory.

    Tools are co-located with their owning skill:
      harness/skills/{skill_name}/scripts/*.py

    Each tool module must expose: TOOL_NAME, TOOL_DESCRIPTION, TOOL_PARAMS, execute(args).
    """
    def __init__(self, skill_name: str = ""):
        self._tools = []
        self._modules = {}
        if not skill_name:
            return
        scripts_dir = os.path.join(SKILLS_BASE, skill_name, "scripts")
        if not os.path.isdir(scripts_dir):
            return
        for entry in sorted(os.listdir(scripts_dir)):
            if not entry.endswith(".py") or entry.startswith("_"):
                continue
            mod_name = entry[:-3]
            try:
                # Load by file path to avoid polluting sys.modules / name clashes
                spec = _importlib_util.spec_from_file_location(
                    f"_skill_tool_{skill_name}_{mod_name}",
                    os.path.join(scripts_dir, entry))
                if spec is None or spec.loader is None:
                    continue
                _mod = _importlib_util.module_from_spec(spec)
                spec.loader.exec_module(_mod)
            except Exception:
                continue
            if not all(hasattr(_mod, a) for a in ("TOOL_NAME","TOOL_DESCRIPTION","TOOL_PARAMS","execute")):
                continue
            self._modules[_mod.TOOL_NAME] = _mod
            self._tools.append({"type":"function","function":{
                "name":_mod.TOOL_NAME,"description":_mod.TOOL_DESCRIPTION,"parameters":_mod.TOOL_PARAMS}})
    def get_tools(self): return list(self._tools)
    def execute(self, name, args):
        _mod = self._modules.get(name)
        if _mod is None: return json.dumps({"error":f"Unknown tool: {name}"})
        try: return _mod.execute(args)
        except Exception as e: return json.dumps({"error":f"Tool '{name}' failed: {e}"})

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
    return frontmatter, parts[2].strip()


# ---------------------------------------------------------------------------
# SKILL.md loading
# ---------------------------------------------------------------------------

def load_skill_md(skill_name: str) -> str | None:
    """Read harness/skills/{skill_name}/SKILL.md"""
    path = os.path.join(SKILLS_BASE, skill_name, "SKILL.md")
    if not os.path.isfile(path):
        return None
    with open(path, encoding="utf-8") as f:
        return f.read()


def load_skills_from_list(skill_names: list) -> dict:
    """Batch load SKILL.md files, returns {name: content}"""
    result = {}
    for name in skill_names:
        content = load_skill_md(name)
        result[name] = content if content else f"[Skill '{name}' 文档未找到]"
    return result


# ---------------------------------------------------------------------------
# LLM interaction
# ---------------------------------------------------------------------------

def get_env(key: str, default: str = "") -> str:
    return os.environ.get(key, default)


# ---------------------------------------------------------------------------
# Main — LLM while-loop with domain tools
# ---------------------------------------------------------------------------

def execute():
    # ① Read framework-injected environment variables
    user_input     = get_env("AF_INPUT", "")
    dialogue_id    = get_env("AF_DIALOGUE_ID", "")
    session_id     = get_env("AF_SESSION_ID", "")
    exec_skill_key = get_env("AF_EXEC_SKILL_KEY", "")

    databus = DataBusClient()

    # ② Read dispatch manifest from DataBus (key = exec_skill_key, e.g. "dispatch:1")
    #    Manifest: {"skill_name": "echo", "prompt": "...", "load_skills": [...]}
    skill_name = ""
    skill_prompt = ""
    load_skill_names = []

    if exec_skill_key and databus.is_connected():
        raw = databus.get(exec_skill_key)
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
        system_parts.append(f"## Skill: {name}\n\n{content}")
    system_prompt = "\n\n---\n\n".join(system_parts) if system_parts else \
                    "你是一个任务执行 agent。完成指派给你的任务。"

    # ⑦ Construct LLM messages
    messages = [
        {"role": "system", "content": system_prompt},
        {"role": "user",   "content": task_instruction},
    ]

    # ⑧ Get domain tool set from CURRENT skill's scripts/ dir (skill-scoped)
    tool_registry = _LocalToolRegistry(skill_name)
    available_tools = tool_registry.get_tools()

    # ⑨ Read model config
    frontmatter, _ = parse_agent_md(AGENT_MD_PATH)
    model_cfg = frontmatter.get("model", {})
    api_key = model_cfg.get("api_key", "")
    if not api_key or "${" in api_key:
        print(json.dumps({"error": "API key not resolved"}, ensure_ascii=False))
        sys.exit(1)

    # ⑩ LLM while-loop
    max_rounds = int(get_env("AF_SKILL_MAX_ROUNDS", "8"))
    for round_i in range(max_rounds):
        raw = call_llm(
            messages, tools=available_tools,
            model=model_cfg.get("name", "gpt-4o-mini"),
            base_url=model_cfg.get("base_url", ""),
            api_key=model_cfg.get("api_key", ""),
            max_completion_tokens=4000)  # avoid finish=length truncation on long skill outputs

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
                result = tool_registry.execute(name, args)
                messages.append({
                    "role": "tool",
                    "tool_call_id": tc.get("id", name),
                    "content": result,
                })
        else:
            # Task complete → output result
            response_text = msg.get("content", "")
            output = {
                "skill_result": response_text,
                "status": "completed",
                "skill_name": skill_name,
                "rounds_used": round_i + 1,
            }
            # Write result to DataBus for coordinator to read.
            # Write under TWO keys:
            #   skill_result:{skill_name}  — real skill name (e.g. weather_query)
            #   skill_result:skill-runner  — generic key matching AF_COMPLETED_SKILL_NAME
            # Coordinator's collect_skill_results reads by completed_skill name ("skill-runner"),
            # so the generic key MUST be written or coordinator gets empty result → false "not found".
            # Use ensure_ascii=True to avoid DataBus SDK UTF-8 decode failures on Chinese/emoji content.
            if databus.is_connected():
                result_json = json.dumps(output, ensure_ascii=True)
                databus.put(f"skill_result:{skill_name}", result_json)
                databus.put("skill_result:skill-runner", result_json)
            print(json.dumps(output, ensure_ascii=False))
            return

    # Max rounds reached
    output = {
        "skill_result": messages[-1].get("content", "") if messages else "",
        "status": "max_rounds_reached",
        "skill_name": skill_name,
        "rounds_used": max_rounds,
    }
    if databus.is_connected():
        result_json = json.dumps(output, ensure_ascii=True)
        databus.put(f"skill_result:{skill_name}", result_json)
        databus.put("skill_result:skill-runner", result_json)
    print(json.dumps(output, ensure_ascii=False))


if __name__ == "__main__":
    execute()
