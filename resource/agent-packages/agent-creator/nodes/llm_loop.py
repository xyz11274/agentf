#!/usr/bin/env python3
"""Agent Creator — LLM-driven conversational agent package creation.

The LLM (acting as "创建智能体") guides users through natural language
to create a new agent package, generating files in real-time via tools.

Input environment variables (injected by worker):
  AF_INPUT        — from workflow node input "input" (user's message)
  AF_SESSION_ID   — from workflow node input "session_id"
  AF_DIALOGUE_ID  — from workflow node input "dialogue_id"

Reads agent.md for model config + system prompt, then runs a while-True
LLM loop with function calling for the tools:
  - add_file(path, content)   — add/update a file in the in-memory tree
  - delete_file(path)         — delete a file
  - finalize()                — mark creation complete

LLM traces are recorded via TraceRecorder.
"""
import importlib.util
import json
import logging
import os
import re
import sys
import traceback as tb_module

from llm_trace_recorder import TraceRecorder

# Ensure tools directory is on path
_TOOLS_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools")
if _TOOLS_DIR not in sys.path:
    sys.path.insert(0, _TOOLS_DIR)

logger = logging.getLogger("agent_creator")


# ---------------------------------------------------------------------------
# Tool discovery — import tools from tools/ directory
# ---------------------------------------------------------------------------

def _discover_tools() -> dict:
    """Import all tool modules from tools/ and build name → module mapping."""
    tool_map = {}
    tools_dir = _TOOLS_DIR
    if not os.path.isdir(tools_dir):
        logger.warning("tools directory not found: %s", tools_dir)
        return tool_map

    for f in sorted(os.listdir(tools_dir)):
        if not f.endswith(".py") or f in ("__init__.py", "file_store.py"):
            continue
        module_name = f[:-3]
        try:
            spec = importlib.util.spec_from_file_location(
                f"tools.{module_name}",
                os.path.join(tools_dir, f),
            )
            mod = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(mod)
            if hasattr(mod, "TOOL_NAME") and hasattr(mod, "execute"):
                tool_map[mod.TOOL_NAME] = mod
                logger.info("loaded tool: %s → %s", mod.TOOL_NAME, f)
            else:
                logger.warning("skipped %s: missing TOOL_NAME or execute()", f)
        except Exception as e:
            logger.error("failed to load tool %s: %s", f, e)
            tb_module.print_exc(file=sys.stderr)

    return tool_map


def build_tool_defs(tool_map: dict) -> list:
    """Build OpenAI-compatible function-calling definitions from tool modules."""
    defs = []
    for name, mod in tool_map.items():
        defs.append({
            "type": "function",
            "function": {
                "name": getattr(mod, "TOOL_NAME", name),
                "description": getattr(mod, "TOOL_DESCRIPTION", ""),
                "parameters": getattr(mod, "TOOL_PARAMS",
                                       {"type": "object", "properties": {}}),
            },
        })
    return defs


def execute_tool(tool_map: dict, tool_call: dict, messages: list) -> None:
    """Execute a single tool call and append the result message."""
    func = tool_call.get("function", {})
    name = func.get("name", "")
    raw_args = func.get("arguments", "{}")

    if name not in tool_map:
        result = json.dumps({"error": f"未知工具: {name}"}, ensure_ascii=False)
        logger.warning("unknown tool called: %s", name)
    else:
        try:
            params = json.loads(raw_args) if isinstance(raw_args, str) else raw_args
        except json.JSONDecodeError:
            params = {}
            logger.warning("tool %s: failed to parse args: %s", name, raw_args)

        logger.info("executing tool: %s %s", name, raw_args[:200])
        try:
            result = tool_map[name].execute(params)
        except Exception as e:
            result = json.dumps({"error": f"{name} 执行失败: {e}"}, ensure_ascii=False)
            logger.error("tool %s execution error: %s", name, e)
            tb_module.print_exc(file=sys.stderr)

    messages.append({
        "role": "tool",
        "tool_call_id": tool_call["id"],
        "content": result,
    })
    logger.info("tool %s result: %s", name, result[:200])


# ---------------------------------------------------------------------------
# agent.md loading
# ---------------------------------------------------------------------------

def load_agent_md():
    """Load agent.md: YAML frontmatter (config) + body (system prompt)."""
    md_path = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "agent.md")
    with open(md_path, "r", encoding="utf-8") as f:
        content = f.read()
    match = re.match(r"^---\n(.*?)\n---\n(.*)", content, re.DOTALL)
    if not match:
        return {}, content
    front_str, body = match.groups()
    config = {}
    current_section = None
    for line in front_str.split("\n"):
        line = line.rstrip()
        if line.startswith("  ") and current_section:
            key, _, val = line.strip().partition(":")
            config[f"{current_section}.{key.strip()}"] = val.strip()
        elif ":" in line and not line.startswith(" "):
            key, _, val = line.partition(":")
            if val.strip():
                config[key.strip()] = val.strip()
            else:
                current_section = key.strip()
    for k, v in config.items():
        if isinstance(v, str) and v.startswith("${") and v.endswith("}"):
            env_var = v[2:-1]
            config[k] = os.environ.get(env_var, "")
    return config, body.strip()


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s [%(name)s] %(levelname)s %(message)s",
        stream=sys.stderr,
    )

    user_text = os.environ.get("AF_INPUT", "")
    session_id = os.environ.get("AF_SESSION_ID", "")
    dialogue_id = os.environ.get("AF_DIALOGUE_ID", "")

    logger.info("starting agent-creator session=%s dialogue=%s", session_id, dialogue_id)

    # --- Trace recorder (DataBus access for traces + session messages) ---
    recorder = TraceRecorder()
    logger.info("TraceRecorder initialized, connected=%s", recorder.is_connected)

    # --- Load conversation history ---
    history_messages = recorder.read_session_messages() if recorder.is_connected else []
    logger.info("loaded %d history messages from DataBus", len(history_messages))

    # --- Load config + system prompt ---
    config, system_prompt = load_agent_md()
    base_url = config.get("model.base_url", "https://api.openai.com/v1")
    api_key = config.get("model.api_key", "")
    model = config.get("model.name", "gpt-4o-mini")

    # --- Discover tools ---
    tool_map = _discover_tools()
    tool_defs = build_tool_defs(tool_map)
    logger.info("discovered %d tool(s): %s", len(tool_defs),
                [t["function"]["name"] for t in tool_defs])

    # --- Build messages ---
    messages = [
        {"role": "system", "content": system_prompt},
    ]
    messages.extend(history_messages)
    messages.append({"role": "user", "content": user_text})
    logger.info("total messages=%d (system=1 + history=%d + input=1)",
                len(messages), len(history_messages))

    # --- While-True LLM loop ---
    max_iterations = 15
    for i in range(max_iterations):
        current_tools = tool_defs  # always provide tools for function calling
        logger.info("iteration=%d messages=%d tools=%s",
                    i, len(messages), len(tool_defs))

        result = recorder.trace_call(
            messages, model, base_url, api_key,
            tools=current_tools, iteration=i,
        )

        if result.get("_llm_error"):
            logger.error("iteration=%d LLM call failed", i)
            output = {"error": result.get("error", "LLM call failed")}
            break

        choice = result["choices"][0]
        msg = choice["message"]
        finish_reason = choice.get("finish_reason", "")
        response_content = msg.get("content", "")
        messages.append(msg)

        tool_calls = msg.get("tool_calls", [])
        if not tool_calls:
            output = {"response": response_content}
            logger.info("iteration=%d completed (%s), response length=%d",
                        i, finish_reason, len(response_content))
            break

        logger.info("iteration=%d executing %d tool(s)", i, len(tool_calls))
        for tc in tool_calls:
            execute_tool(tool_map, tc, messages)

        # Check if finalized via tool_map (single authority for tool access)
        finalize_mod = tool_map.get("finalize")
        if finalize_mod and getattr(finalize_mod, "is_finalized", lambda: False)():
            logger.info("agent creation finalized at iteration=%d", i)
            # Don't break here — let LLM respond to user about completion
    else:
        output = {"response": "处理超时，请重试。"}
        logger.warning("max_iterations=%d reached", max_iterations)

    print(json.dumps(output, ensure_ascii=False))

    # Save conversation history for multi-turn (exclude system prompt)
    if recorder.is_connected:
        messages_to_save = messages[1:]  # remove system prompt
        recorder.write_session_messages(messages_to_save)

    logger.info("finished agent-creator session=%s", session_id)
    sys.exit(0)


if __name__ == "__main__":
    main()
