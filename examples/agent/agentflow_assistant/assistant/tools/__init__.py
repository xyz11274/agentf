"""
Tool Registry — auto-discovers and loads all tools from this directory.

Each tool module must export:
    TOOL_NAME: str          — unique tool identifier
    TOOL_DESCRIPTION: str   — description for LLM function calling
    TOOL_PARAMS: dict       — JSON Schema for tool parameters

    def execute(tool_input: dict) -> str:
        '''Execute the tool and return a JSON string result.'''
"""

import importlib
import json
import os
import pkgutil
import sys
from types import ModuleType

_TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))

_registry: dict[str, ModuleType] = {}
_llm_tools: list[dict] = []


def _discover():
    """Scan the tools directory and load all valid tool modules."""
    global _registry, _llm_tools
    _registry = {}
    _llm_tools = []

    for importer, mod_name, is_pkg in pkgutil.iter_modules([_TOOLS_DIR]):
        if mod_name.startswith("_"):
            continue  # skip __init__.py

        try:
            mod = importlib.import_module(f"tools.{mod_name}")
        except Exception as e:
            print(f"[TOOL_LOADER] Failed to load tool '{mod_name}': {e}", file=sys.stderr)
            continue

        # Validate required exports
        for attr in ("TOOL_NAME", "TOOL_DESCRIPTION", "TOOL_PARAMS", "execute"):
            if not hasattr(mod, attr):
                print(f"[TOOL_LOADER] Tool '{mod_name}' missing '{attr}', skipping",
                      file=sys.stderr)
                break
        else:
            name = mod.TOOL_NAME
            _registry[name] = mod
            _llm_tools.append({
                "type": "function",
                "function": {
                    "name": name,
                    "description": mod.TOOL_DESCRIPTION,
                    "parameters": mod.TOOL_PARAMS,
                }
            })
            print(f"[TOOL_LOADER] Registered tool: {name}", file=sys.stderr)


# ── Public API ────────────────────────────────────────────────

def get_tools() -> list[dict]:
    """Return the list of LLM function-calling tool definitions."""
    if not _registry:
        _discover()
    return list(_llm_tools)


def execute(tool_name: str, tool_input: dict) -> str:
    """Execute a tool by name and return its JSON result string."""
    if not _registry:
        _discover()

    mod = _registry.get(tool_name)
    if mod is None:
        return json.dumps({"error": f"Unknown tool: {tool_name}"}, ensure_ascii=False)

    try:
        return mod.execute(tool_input)
    except Exception as e:
        return json.dumps({"error": f"Tool '{tool_name}' failed: {e}"}, ensure_ascii=False)
