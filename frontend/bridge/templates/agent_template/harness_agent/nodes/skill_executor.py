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

import importlib as _importlib
import importlib.util as _importlib_util
import json
import os
import re
import sys
import time
from pathlib import Path

import yaml

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from databus_client import DataBusClient
from llm_call import call_llm, estimate_message_tokens, compact_messages, get_model_max_tokens, get_compaction_threshold
from observability import emit_event
from round_reminder import build_round_reminder
from prompts import (
    SKILL_FALLBACK_SYSTEM,
    SKILL_SECTION_TEMPLATE,
    SKILL_NOT_FOUND_TEMPLATE,
    SKILL_RECOVERY_PROMPT,
)

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
TOOLS_DIR = os.path.join(AGENT_PACKAGE_DIR, "harness", "tools")

class _LocalToolRegistry:
    """Loads domain tools from global harness/tools/ AND skill-specific scripts/.

    Global tools (harness/tools/*.py) provide atomic capabilities like bash,
    read_file, write_file, web_fetch — available to ALL skills.

    Skill-scoped tools (harness/skills/{skill_name}/scripts/*.py) provide
    domain-specific tools — only available to the owning skill.

    Each tool module must expose: TOOL_NAME, TOOL_DESCRIPTION, TOOL_PARAMS, execute(args).
    Global tools are loaded first; skill-scoped tools override same-named globals.
    """
    def __init__(self, skill_name: str = ""):
        self._tools = []
        self._modules = {}

        # 1. Load global tools from harness/tools/
        if os.path.isdir(TOOLS_DIR):
            _load_dir(self, TOOLS_DIR)

        # 2. Load skill-scoped tools from harness/skills/{skill_name}/scripts/
        if skill_name:
            scripts_dir = os.path.join(SKILLS_BASE, skill_name, "scripts")
            if os.path.isdir(scripts_dir):
                _load_dir(self, scripts_dir, prefix=f"_skill_{skill_name}_")

    def get_tools(self):
        return list(self._tools)

    def execute(self, name, args):
        _mod = self._modules.get(name)
        if _mod is None:
            return json.dumps({"error": f"Unknown tool: {name}"})
        try:
            return _mod.execute(args)
        except Exception as e:
            return json.dumps({"error": f"Tool '{name}' failed: {e}"})


def _load_dir(registry: _LocalToolRegistry, directory: str, prefix: str = ""):
    """Load all valid tool modules from a directory into the registry."""
    for entry in sorted(os.listdir(directory)):
        if not entry.endswith(".py") or entry.startswith("_"):
            continue
        mod_name = entry[:-3]
        try:
            spec = _importlib_util.spec_from_file_location(
                f"_tool_{prefix}{mod_name}",
                os.path.join(directory, entry))
            if spec is None or spec.loader is None:
                continue
            _mod = _importlib_util.module_from_spec(spec)
            spec.loader.exec_module(_mod)
        except Exception:
            continue
        if not all(hasattr(_mod, a) for a in ("TOOL_NAME", "TOOL_DESCRIPTION", "TOOL_PARAMS", "execute")):
            continue
        registry._modules[_mod.TOOL_NAME] = _mod
        registry._tools.append({"type": "function", "function": {
            "name": _mod.TOOL_NAME, "description": _mod.TOOL_DESCRIPTION, "parameters": _mod.TOOL_PARAMS}})


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
    """Read harness/skills/{skill_name}/SKILL.md and replace AgentFoundry template vars.

    AgentFoundry materializes placeholders like ${SKILL_DIR} and ${SKILLS_DIR}
    at mount time.  Our V3 coordinator doesn't have a materialization step,
    so we substitute them here with the actual filesystem paths.
    """
    path = os.path.join(SKILLS_BASE, skill_name, "SKILL.md")
    if not os.path.isfile(path):
        return None
    with open(path, encoding="utf-8") as f:
        content = f.read()
    # Replace AgentFoundry template variables
    skill_dir = os.path.join(SKILLS_BASE, skill_name)
    content = content.replace("${SKILL_DIR}", skill_dir)
    content = content.replace("${SKILLS_DIR}", SKILLS_BASE)
    return content


def load_skills_from_list(skill_names: list) -> dict:
    """Batch load SKILL.md files, returns {name: content}"""
    result = {}
    for name in skill_names:
        content = load_skill_md(name)
        result[name] = content if content else SKILL_NOT_FOUND_TEMPLATE.format(name=name)
    return result


# ---------------------------------------------------------------------------
# LLM interaction
# ---------------------------------------------------------------------------

def get_env(key: str, default: str = "") -> str:
    return os.environ.get(key, default)


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


def extract_artifact_paths(text: str) -> list[str]:
    """Extract generated image URLs/paths from a tool result.

    Handles three formats:
      - HTTP(S) URLs:         https://cdn.example/look.jpg
      - Absolute paths:       /home/user/outfit.png
      - Relative paths:       .agentfoundry/.../file.png, assets/outfit.png

    Relative paths are resolved against AGENT_PACKAGE_DIR (the cwd for tool execution).
    """
    pattern = (
        r"(?:"
        r"https?://[^\s\"'\\]+"          # http(s) URLs
        r"|/[^\s\"'\\]+"                 # absolute paths
        r"|(?:\.\.?)?[^\s\"'\\:]+/[^\s\"'\\]+"  # relative paths with at least one /
        r")"
        r"\.(?:png|jpe?g|webp|gif)"
    )
    candidates = re.findall(pattern, text, flags=re.IGNORECASE)
    seen: list[str] = []
    for item in candidates:
        if item.startswith(("http://", "https://")):
            seen.append(item)
        elif item.startswith("/"):
            # Absolute path — use as-is
            if os.path.isfile(item):
                seen.append(item)
        else:
            # Relative path — resolve against AGENT_PACKAGE_DIR
            abs_path = os.path.normpath(os.path.join(AGENT_PACKAGE_DIR, item))
            if os.path.isfile(abs_path):
                seen.append(abs_path)
    return list(dict.fromkeys(seen))


def _ensure_artifacts_in_package(artifacts: list[str]) -> list[str]:
    """Copy artifacts into package outputs/ and return updated paths.

    The Bridge artifact endpoint only serves files under agent_packages_path.
    This function copies ALL artifact files (whether inside or outside the
    package) to AGENT_PACKAGE_DIR/outputs/, giving a single predictable
    location regardless of where the LLM saved them. Avoids issues with:
      - Hidden directories (.agentfoundry/) in the URL path
      - Files saved outside the package (/tmp/...)
      - Path resolution mismatches between daemon and Bridge processes
    """
    result = []
    pkg_root = Path(AGENT_PACKAGE_DIR).resolve()
    outputs_dir = pkg_root / "outputs"
    outputs_dir.mkdir(parents=True, exist_ok=True)
    for path in artifacts:
        p = Path(path).resolve()
        if not p.is_file():
            continue  # skip non-existent files rather than returning dead URLs
        try:
            p.relative_to(pkg_root)
        except ValueError:
            pass
        else:
            # Package-local artifacts are already publishable by Bridge. Keep
            # their directory hierarchy so HTML/CSS relative references and
            # same-named files remain stable.
            result.append(str(p))
            continue
        dest = outputs_dir / p.name
        # Deduplicate: if same filename already copied, add a suffix
        if dest.exists() and dest.read_bytes() != p.read_bytes():
            stem = dest.stem
            suffix = dest.suffix
            counter = 1
            while True:
                alt = outputs_dir / f"{stem}_{counter}{suffix}"
                if not alt.exists():
                    dest = alt
                    break
                counter += 1
        if not dest.exists():
            try:
                dest.write_bytes(p.read_bytes())
            except OSError:
                continue  # skip files that can't be copied
        result.append(str(dest))
    return result


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
    # model can be a plain string (e.g. "MiniMax-M3") or a dict {name: "..."}
    model_name = model_cfg if isinstance(model_cfg, str) else model_cfg.get("name", "")
    # agent.md 为模型名第一来源，未声明时 fallback 到 AF_LLM_MODEL env，再兜底 gpt-4o-mini
    if not model_name:
        model_name = get_env("AF_LLM_MODEL") or "gpt-4o-mini"

    # API key and base URL come from env vars (not agent.md frontmatter).
    # Priority: AF_LLM_API_KEY > API_KEY, AF_LLM_BASE_URL > default.
    api_key = get_env("AF_LLM_API_KEY") or get_env("API_KEY", "")
    if not api_key or "${" in api_key:
        print(json.dumps({"error": "API key not resolved"}, ensure_ascii=False))
        sys.exit(1)
    base_url = get_env("AF_LLM_BASE_URL", "")

    # ── flow debug ──
    emit_event("skill_executor.trigger", {
        "exec_skill_key": exec_skill_key,
        "skill_name": skill_name,
        "load_skills": load_skill_names,
        "model": model_name,
        "max_rounds": max_rounds,
    })
    print(f"[flow] skill_executor triggered: session={session_id} dialogue={dialogue_id}", file=sys.stderr)
    print(f"[flow]   exec_skill_key={exec_skill_key} skill_name={skill_name}", file=sys.stderr)
    print(f"[flow]   skill_prompt={skill_prompt[:80] if skill_prompt else '(none)'}", file=sys.stderr)
    print(f"[flow]   load_skills={load_skill_names}", file=sys.stderr)
    print(f"[flow]   tools_available={[t['function']['name'] for t in available_tools]}", file=sys.stderr)

    # ⑩ LLM while-loop
    max_rounds = int(get_env("AF_MAX_SKILL_TASK_ITERATIONS",
                    get_env("AF_SKILL_MAX_ROUNDS", "8")))
    # Node-level timeout (injected by framework as AF_NODE_EXECUTION_TIMEOUT);
    # fallback to node default 240s if absent.
    node_timeout_sec = int(get_env("AF_NODE_EXECUTION_TIMEOUT", "240"))
    completion_budget = int(get_env("AF_SKILL_MAX_COMPLETION_TOKENS", "8192"))
    recovery_attempts = 0
    max_recovery_attempts = int(get_env("AF_SKILL_LLM_RECOVERY_ATTEMPTS", "2"))
    generated_artifacts = extract_artifact_paths(task_instruction)
    generated_artifacts = _ensure_artifacts_in_package(generated_artifacts)

    _turn_start = time.time()
    for round_i in range(max_rounds):
        elapsed = time.time() - _turn_start
        emit_event("skill_executor.llm_round_start", {
            "round": round_i + 1,
            "max_rounds": max_rounds,
            "messages": len(messages),
            "model": model_name,
            "tools": bool(available_tools),
            "budget": completion_budget,
            "elapsed_s": round(elapsed, 1),
        })
        print(
            f"[flow]   LLM round={round_i+1}/{max_rounds} msgs={len(messages)} "
            f"tools={bool(available_tools)} model={model_name} budget={completion_budget} "
            f"elapsed={elapsed:.0f}s",
            file=sys.stderr,
        )

        # ── Progress: push round info to frontend ──
        # "第 X 轮：调用 LLM…" → 用户看到"正在等 API"而非"卡了"
        _round_start = time.time()
        print(json.dumps({
            "response": f"{skill_name} 第 {round_i+1}/{max_rounds} 轮：调用 LLM…",
        }, ensure_ascii=False))

        # ── Per-round budget reminder (delegated to shared module) ──
        reminder = build_round_reminder(
            round_i=round_i,
            max_rounds=max_rounds,
            elapsed=elapsed,
            node_timeout_sec=node_timeout_sec,
        )
        if reminder:
            messages.append(reminder)

        # ── Token budget: compact messages if exceeding threshold ──
        model_max_tokens = get_model_max_tokens()
        compaction_threshold = get_compaction_threshold()
        before_tok = estimate_message_tokens(messages)
        compacted = compact_messages(messages, model_max_tokens, compaction_threshold)
        after_tok = estimate_message_tokens(compacted)
        if compacted is not messages:
            print(f"[flow]   compaction: {before_tok}→{after_tok} tok "
                  f"(threshold={int(model_max_tokens * compaction_threshold)}), "
                  f"dropped {len(messages) - len(compacted)} msgs", file=sys.stderr)
            messages = compacted

        raw = call_llm(
            messages, tools=available_tools,
            model=model_name,
            base_url=base_url,
            api_key=api_key,
            max_completion_tokens=completion_budget)

        if raw.get("_llm_error"):
            err_msg = raw.get("error", "LLM call failed")
            print(f"[flow]   ⚠️  LLM error after call_llm internal retries: {err_msg}", file=sys.stderr)
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
                databus.put(f"skill_result:{skill_name}", result_json)
                databus.put("skill_result:skill-runner", result_json)
            print(json.dumps(output, ensure_ascii=False))
            return

        choice = raw.get("choices", [{}])[0]
        msg = choice.get("message", {})
        tool_calls = msg.get("tool_calls")

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
                print(json.dumps({
                    "response": f"{skill_name} 正在恢复第 {round_i+1} 轮…",
                }, ensure_ascii=False))
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
                databus.put(f"skill_result:{skill_name}", result_json)
                databus.put("skill_result:skill-runner", result_json)
            print(json.dumps(output, ensure_ascii=False))
            return

        if tool_calls:
            tool_names = [tc.get("function", {}).get("name", "?") for tc in tool_calls]
            print(f"[flow]   round={round_i+1} → tool_calls: {tool_names}", file=sys.stderr)
            # ── Progress: tool call details ──
            print(json.dumps({
                "response": f"{skill_name} → 正在调用 {', '.join(tool_names)}",
            }, ensure_ascii=False))
            messages.append(msg)
            for tc in tool_calls:
                func = tc.get("function", {})
                name = func.get("name", "")
                args = json.loads(func.get("arguments", "{}"))
                # Log tool call params (truncate to 200 chars)
                args_preview = json.dumps(args, ensure_ascii=False)[:200]
                print(f"[flow]   tool_call: {name} args={args_preview}", file=sys.stderr)
                result = tool_registry.execute(name, args)
                for artifact in extract_artifact_paths(result):
                    if artifact not in generated_artifacts:
                        generated_artifacts.append(artifact)
                # Relocate artifacts written outside agent package into package outputs/
                generated_artifacts = _ensure_artifacts_in_package(generated_artifacts)
                # Log tool result (truncate to 300 chars)
                result_preview = result[:300]
                print(f"[flow]   tool_result: {name} => {result_preview}", file=sys.stderr)
                messages.append({
                    "role": "tool",
                    "tool_call_id": tc.get("id", name),
                    "content": result,
                })
        else:
            # Task complete → output result
            # ── Progress: LLM returned final answer ──
            print(json.dumps({
                "response": f"{skill_name} 汇总结果…",
            }, ensure_ascii=False))
            response_text = msg.get("content", "")
            output = {
                "skill_result": response_text,
                "status": "completed",
                "skill_name": skill_name,
                "rounds_used": round_i + 1,
                "artifacts_json": json.dumps(generated_artifacts, ensure_ascii=False),
            }
            # Write result to DataBus for coordinator to read.
            # Write under TWO keys:
            #   skill_result:{skill_name}  — real skill name (e.g. weather_query)
            #   skill_result:skill-runner  — generic key matching AF_COMPLETED_SKILL_NAME
            # Coordinator's collect_skill_results reads by completed_skill name ("skill-runner"),
            # so the generic key MUST be written or coordinator gets empty result → false "not found".
            # Use ensure_ascii=True to avoid DataBus SDK UTF-8 decode failures on Chinese/emoji content.
            emit_event("skill_executor.completed", {
                "round": round_i + 1,
                "total_rounds": round_i + 1,
                "skill_name": skill_name,
                "status": "completed",
                "response_length": len(response_text),
                "elapsed_s": round(time.time() - _turn_start, 1),
            })
            print(f"[flow]   result: skill={skill_name} status={output.get('status','completed')} rounds={round_i+1}", file=sys.stderr)
            if databus.is_connected():
                result_json = json.dumps(output, ensure_ascii=True)
                databus.put(f"skill_result:{skill_name}", result_json)
                databus.put("skill_result:skill-runner", result_json)
            print(json.dumps(output, ensure_ascii=False))
            return

    # Max rounds reached
    emit_event("skill_executor.max_rounds_reached", {
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
        databus.put(f"skill_result:{skill_name}", result_json)
        databus.put("skill_result:skill-runner", result_json)
    print(json.dumps(output, ensure_ascii=False))


if __name__ == "__main__":
    execute()
