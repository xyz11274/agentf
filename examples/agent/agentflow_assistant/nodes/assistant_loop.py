#!/usr/bin/env python3
"""
Assistant Loop Node — LLM think-and-act loop for AgentFlow Assistant.

Single Python process with an internal while True loop:
  1. Reads agent.md for system prompt (body) and config (YAML frontmatter)
  2. Reads conversation history (messages list) from DataBus ctx namespace
  3. LLM decides: call a tool? or respond?
  4. On response: writes updated messages back to DataBus for next turn

Environment variables (injected by the worker):
  AF_INPUT               — The user's original request
  AF_DIALOGUE_ID         — Current dialogue ID
  AF_SESSION_ID          — Current session ID
  AF_DATABUS_UDS_FILE    — UDS socket for runtime DataBus access
"""

import json
import os
import re
import sys
import yaml
from dotenv import load_dotenv

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from databus_client import DataBusClient


NODES_DIR = os.path.dirname(os.path.abspath(__file__))
AGENT_PACKAGE_DIR = os.path.dirname(NODES_DIR)
AGENT_MD_PATH = os.path.join(AGENT_PACKAGE_DIR, "agent.md")


# --- Load tools from assistant/tools/ directory ---
TOOLS_DIR = os.path.join(AGENT_PACKAGE_DIR, "assistant", "tools")
sys.path.insert(0, os.path.join(AGENT_PACKAGE_DIR, "assistant"))
import tools as tool_registry


# ---------------------------------------------------------------------------
# agent.md parsing
# ---------------------------------------------------------------------------

RE_ENV_VAR = re.compile(r"\$\{(\w+)\}")


def _expand_env(value):
    """Replace ${VAR} with os.environ value (recursive on strings/dicts/lists)."""
    if isinstance(value, str):
        return RE_ENV_VAR.sub(lambda m: os.environ.get(m.group(1), m.group(0)), value)
    if isinstance(value, dict):
        return {k: _expand_env(v) for k, v in value.items()}
    if isinstance(value, list):
        return [_expand_env(v) for v in value]
    return value


def parse_agent_md(path: str) -> tuple:
    """Read agent.md, return (frontmatter_dict, body_text)."""
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
# Tool definitions (auto-discovered tools for assistant)
# ---------------------------------------------------------------------------

def build_tools_from_config(frontmatter: dict) -> list:
    """Build function-calling tools from the auto-discovered tool registry."""
    return tool_registry.get_tools()


# ---------------------------------------------------------------------------
# LLM interaction
# ---------------------------------------------------------------------------

def get_env(key: str, default: str = "") -> str:
    return os.environ.get(key, default)


def call_llm(messages: list, tools: list | None = None,
             model: str = "gpt-4o-mini",
             base_url: str = "",
             api_key: str = "") -> dict:
    """Call the LLM API and return the raw response dict."""
    import requests
    url = base_url.rstrip("/")
    if not url.endswith("/chat/completions"):
        url += "/chat/completions"

    headers = {
        "Content-Type": "application/json",
        "Authorization": f"Bearer {api_key}",
    }
    body = {
        "model": model,
        "messages": messages,
        "max_completion_tokens": 2000,
    }
    if tools:
        body["tools"] = tools
        body["tool_choice"] = "auto"
    try:
        resp = requests.post(url, json=body, headers=headers, timeout=60)
        resp.raise_for_status()
        return resp.json()
    except Exception as e:
        return {"error": f"LLM call failed: {e}", "_llm_error": True}


# ---------------------------------------------------------------------------
# Main — internal while True loop
# ---------------------------------------------------------------------------

def execute():
    # Load .env from agent package root (CWD = package dir when run by daemon)
    load_dotenv()

    user_input = get_env("AF_INPUT", "")
    dialogue_id = get_env("AF_DIALOGUE_ID", "")
    session_id = get_env("AF_SESSION_ID", "")

    frontmatter, system_prompt = parse_agent_md(AGENT_MD_PATH)
    model_cfg = frontmatter.get("model", {})

    tools = build_tools_from_config(frontmatter)

    api_key = model_cfg.get("api_key", "")
    if not api_key or "${" in api_key:
        print(json.dumps({"error": "API key not resolved. "
                          "Set API_KEY env var or configure agent.md model.api_key."},
                          ensure_ascii=False))
        sys.exit(1)

    databus = DataBusClient()
    history_messages = databus.read_messages() if databus.is_connected() else []

    messages = [{"role": "system", "content": system_prompt or
                 "You are an AgentFlow assistant."}]
    messages.extend(history_messages)
    messages.append({"role": "user", "content": user_input})

    while True:
        raw = call_llm(
            messages, tools=tools,
            model=model_cfg.get("name", "gpt-4o-mini"),
            base_url=model_cfg.get("base_url", ""),
            api_key=model_cfg.get("api_key", ""))

        if raw.get("_llm_error"):
            err_msg = raw.get("error", "LLM call failed")
            print(f"[ERROR] {err_msg}", file=sys.stderr)
            print(json.dumps({"response": err_msg}, ensure_ascii=False))
            sys.exit(0)

        choice = raw.get("choices", [{}])[0]
        msg = choice.get("message", {})
        tool_calls = msg.get("tool_calls")

        if tool_calls:
            tc = tool_calls[0]
            func = tc.get("function", {})
            name = func.get("name", "")
            try:
                args = json.loads(func.get("arguments", "{}"))
            except json.JSONDecodeError:
                args = {}
            result = tool_registry.execute(name, args)
            messages.append({
                "role": "tool",
                "tool_call_id": name,
                "content": result,
            })

        else:
            response_text = msg.get("content", "")

            print(json.dumps({"response": response_text}, ensure_ascii=False))

            messages_to_save = messages[1:]
            if databus.is_connected():
                databus.write_messages(messages_to_save)

            break


if __name__ == "__main__":
    execute()
