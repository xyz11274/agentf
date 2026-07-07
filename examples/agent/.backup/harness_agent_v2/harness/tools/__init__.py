"""
Tool Registry — auto-discovers and loads all tools from this directory.

Each tool module must export:
    TOOL_NAME: str          — unique tool identifier
    TOOL_DESCRIPTION: str   — description for LLM function calling
    TOOL_PARAMS: dict       — JSON Schema for tool parameters

    def execute(tool_input: dict) -> str:
        '''Execute the tool and return a JSON string result.'''

MCP tools are discovered from .mcp.json and registered alongside local tools.
MCP tool names follow the convention: mcp__{server_name}__{tool_name}
"""

import atexit
import importlib
import json
import os
import pkgutil
import sys
from types import ModuleType

_TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))

_registry: dict[str, ModuleType] = {}
_llm_tools: list[dict] = []

# ── MCP Bridge (lazy loaded) ─────────────────────────────────
_MCP_BRIDGES: dict[str, "MCPBridge"] = {}
_MCP_LLM_TOOLS: list[dict] = []
_MCP_LOADED = False


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
            import sys
            print(f"[TOOL_LOADER] Failed to load tool '{mod_name}': {e}", file=sys.stderr)
            continue

        # Validate required exports
        for attr in ("TOOL_NAME", "TOOL_DESCRIPTION", "TOOL_PARAMS", "execute"):
            if not hasattr(mod, attr):
                import sys
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
            import sys
            print(f"[TOOL_LOADER] Registered tool: {name}", file=sys.stderr)


def _locate_mcp_config() -> str | None:
    """Find the .mcp.json file relative to the agent package."""
    # __file__ = .../examples/agent/harness_agent/harness/tools/__init__.py
    # 6x dirname = project root
    project_root = os.path.dirname(os.path.dirname(os.path.dirname(
        os.path.dirname(os.path.dirname(os.path.dirname(
            os.path.abspath(__file__)))))))
    # 3x dirname = agent package root
    agent_root = os.path.dirname(os.path.dirname(os.path.dirname(
        os.path.abspath(__file__))))
    candidates = [
        os.path.join(agent_root, ".mcp.json"),
        os.path.join(project_root, ".mcp.json"),
    ]
    for path in candidates:
        if os.path.isfile(path):
            return path
    return None


def _load_mcp_tools():
    """Discover MCP servers and register their tools."""
    global _MCP_BRIDGES, _MCP_LLM_TOOLS, _MCP_LOADED
    if _MCP_LOADED:
        return
    _MCP_LOADED = True

    from tools.mcp_bridge import MCPBridge, MCPServerConfig, discover_servers

    config_path = _locate_mcp_config()
    if not config_path:
        return  # No MCP config found, skip

    servers = discover_servers(config_path)
    if not servers:
        return

    for svr in servers:
        bridge = MCPBridge(svr)
        if not bridge.ensure_ready():
            print(f"[MCP] Skipping server '{svr.name}': connection failed",
                  file=sys.stderr)
            continue

        tools = bridge.load_tools()
        if not tools:
            print(f"[MCP] Server '{svr.name}' returned no tools, shutting down",
                  file=sys.stderr)
            bridge.shutdown()
            continue

        _MCP_BRIDGES[svr.name] = bridge
        _MCP_LLM_TOOLS.extend(tools)
        for t in tools:
            name = t["function"]["name"]
            print(f"[MCP] Registered tool: {name}", file=sys.stderr)

    # Register cleanup
    atexit.register(_shutdown_mcp_servers)


def _shutdown_mcp_servers():
    for name, bridge in _MCP_BRIDGES.items():
        try:
            bridge.shutdown()
        except Exception:
            pass


# ── Public API ────────────────────────────────────────────────

def get_tools() -> list[dict]:
    """Return the list of LLM function-calling tool definitions (local + MCP)."""
    if not _registry:
        _discover()
    if not _MCP_LOADED:
        _load_mcp_tools()
    return list(_llm_tools) + list(_MCP_LLM_TOOLS)


def execute(tool_name: str, tool_input: dict) -> str:
    """Execute a tool by name and return its JSON result string.

    Routes to:
      - local tools (bash, echo, read_file, write_file, web_fetch, ...)
      - MCP tools (mcp__server__tool)
    """
    if not _registry:
        _discover()
    if not _MCP_LOADED:
        _load_mcp_tools()

    # ── MCP tool routing ──
    if tool_name.startswith("mcp__"):
        parts = tool_name.split("__", 2)
        if len(parts) != 3:
            return json.dumps({"error": f"Invalid MCP tool name: {tool_name}",
                               "tool": tool_name}, ensure_ascii=False)
        _, server_name, mcp_tool_name = parts
        bridge = _MCP_BRIDGES.get(server_name)
        if bridge is None:
            return json.dumps({"error": f"MCP server not found: {server_name}",
                               "tool": tool_name}, ensure_ascii=False)
        try:
            result = bridge.call_tool(mcp_tool_name, tool_input)
            return json.dumps(result, ensure_ascii=False)
        except Exception as e:
            return json.dumps({"error": f"MCP tool '{tool_name}' failed: {e}",
                               "tool": tool_name}, ensure_ascii=False)

    # ── Local tool routing (original) ──
    mod = _registry.get(tool_name)
    if mod is None:
        return json.dumps({"error": f"Unknown tool: {tool_name}"}, ensure_ascii=False)

    try:
        return mod.execute(tool_input)
    except Exception as e:
        return json.dumps({"error": f"Tool '{tool_name}' failed: {e}"}, ensure_ascii=False)
