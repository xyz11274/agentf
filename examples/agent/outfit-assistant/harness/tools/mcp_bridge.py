"""
MCP Bridge — connects harness_agent to MCP servers via official mcp SDK.

Gives the agent access to MCP ecosystem tools (search, database, APIs, etc.)
alongside local harness/tools/*.py tools.

Transport types (from .mcp.json server config):
  - "stdio":           Local subprocess (npx/uv run server)
  - "sse":             Remote SSE endpoint (legacy)
  - "streamable-http": Remote HTTP endpoint (recommended)
  - "url":             Auto-detect SSE vs streamable-http by endpoint path

Usage:
  pip install "mcp>=1.27,<2"
"""

import asyncio
import json
import os
import sys
from contextlib import AsyncExitStack
from dataclasses import dataclass, field
from typing import Any


# ── SSL-unverified httpx client factory for MCP connections ─────────
# Skip certificate verification to avoid cert issues in various
# environments (self-signed, internal CA, proxy interception, etc.).
def _unverified_http_client(
    headers: dict[str, str] | None = None,
    timeout: Any = None,
    auth: Any = None,
) -> Any:
    """Create an httpx.AsyncClient with SSL verification disabled."""
    try:
        import httpx
    except ImportError:
        raise ImportError("httpx is required for MCP remote connections")

    kwargs: dict[str, Any] = {
        "verify": False,
        "follow_redirects": True,
    }
    if headers is not None:
        kwargs["headers"] = headers
    if timeout is not None:
        kwargs["timeout"] = timeout
    if auth is not None:
        kwargs["auth"] = auth
    return httpx.AsyncClient(**kwargs)


# ── Configuration Model ──────────────────────────────────────

@dataclass
class MCPServerConfig:
    """MCP Server config, parsed from .mcp.json mcpServers entry."""
    name: str
    transport_type: str = "stdio"       # stdio | sse | streamable-http | url
    command: str = ""                   # stdio only
    args: list = field(default_factory=list)       # stdio only
    env: dict = field(default_factory=dict)        # stdio only
    url: str = ""                       # sse / streamable-http / url
    headers: dict = field(default_factory=dict)    # sse / streamable-http


# ── Server Discovery ─────────────────────────────────────────

def discover_servers(*config_paths: str) -> list[MCPServerConfig]:
    """Scan config paths for .mcp.json, return all MCP server configs."""
    servers = []
    seen = set()
    for path in config_paths:
        if not os.path.isfile(path):
            continue
        try:
            with open(path) as f:
                data = json.load(f)
        except (json.JSONDecodeError, PermissionError):
            continue
        for name, cfg in data.get("mcpServers", {}).items():
            if name in seen:
                continue
            seen.add(name)
            tp = cfg.get("type", "stdio")
            if tp in ("sse", "streamable-http", "url"):
                servers.append(MCPServerConfig(
                    name=name,
                    transport_type=tp,
                    url=cfg.get("url", ""),
                    headers=cfg.get("headers", {}),
                ))
            else:  # stdio (default)
                servers.append(MCPServerConfig(
                    name=name,
                    transport_type="stdio",
                    command=cfg.get("command", ""),
                    args=cfg.get("args", []),
                    env={k: str(v) for k, v in cfg.get("env", {}).items()},
                ))
    return servers


# ── Async MCP Bridge (internal) ──────────────────────────────

class _AsyncMCPBridge:
    """Async implementation of MCP server connection and tool operations."""

    def __init__(self, config: MCPServerConfig):
        self._config = config
        self._session = None
        self._stack: AsyncExitStack | None = None
        self._runnable = False

    async def ensure_ready(self) -> bool:
        if self._runnable:
            return True
        return await self._connect()

    async def _connect(self) -> bool:
        try:
            self._stack = AsyncExitStack()
            cfg = self._config

            if cfg.transport_type == "stdio":
                from mcp.client.stdio import stdio_client
                from mcp import StdioServerParameters

                params = StdioServerParameters(
                    command=cfg.command,
                    args=cfg.args,
                    env={**os.environ, **cfg.env} if cfg.env else None,
                )
                streams = await self._stack.enter_async_context(stdio_client(params))

            elif cfg.transport_type in ("sse", "url"):
                from mcp.client.sse import sse_client
                streams = await self._stack.enter_async_context(
                    sse_client(cfg.url, headers=cfg.headers or None,
                               httpx_client_factory=_unverified_http_client)
                )

            elif cfg.transport_type == "streamable-http":
                from mcp.client.streamable_http import streamablehttp_client
                streams = await self._stack.enter_async_context(
                    streamablehttp_client(cfg.url, headers=cfg.headers or None,
                                          httpx_client_factory=_unverified_http_client)
                )
            else:
                raise ValueError(f"Unsupported transport: {cfg.transport_type}")

            from mcp import ClientSession
            read, write = streams
            self._session = await self._stack.enter_async_context(
                ClientSession(read, write)
            )
            await self._session.initialize()
            self._runnable = True
            return True

        except Exception as e:
            print(f"[MCP] Failed to connect '{cfg.name}': {e}", file=sys.stderr)
            await self._cleanup()
            return False

    async def _cleanup(self):
        self._runnable = False
        self._session = None
        if self._stack:
            await self._stack.aclose()
            self._stack = None

    async def shutdown(self):
        await self._cleanup()

    async def list_tools(self) -> list[dict]:
        if not self._runnable or not self._session:
            return []
        try:
            result = await self._session.list_tools()
            tools = []
            for t in result.tools:
                tools.append({
                    "type": "function",
                    "function": {
                        "name": f"mcp__{self._config.name}__{t.name}",
                        "description": t.description or "",
                        "parameters": t.inputSchema if hasattr(t, 'inputSchema') else {},
                    }
                })
            return tools
        except Exception as e:
            print(f"[MCP] list_tools failed for '{self._config.name}': {e}",
                  file=sys.stderr)
            return []

    async def call_tool(self, tool_name: str, arguments: dict) -> dict:
        if not self._runnable or not self._session:
            return {"error": f"MCP server '{self._config.name}' not connected"}
        try:
            result = await self._session.call_tool(tool_name, arguments)
            texts = []
            for content in (result.content or []):
                if hasattr(content, 'text'):
                    texts.append(content.text)
                elif hasattr(content, 'type') and content.type == 'text':
                    texts.append(content.text or '')
            return {
                "result": "\n".join(texts),
                "isError": getattr(result, 'isError', False),
                "tool": tool_name,
            }
        except Exception as e:
            return {"error": f"MCP tool '{tool_name}' failed: {e}"}


# ── Sync MCP Bridge (public API) ─────────────────────────────

class MCPBridge:
    """Synchronous wrapper around _AsyncMCPBridge.

    Manages a dedicated event loop for the bridge's lifetime,
    so that async resources (stdio_client, AsyncExitStack)
    are managed within a single consistent event loop.
    Safe to use from synchronous tool_registry.execute() context.
    """

    def __init__(self, config: MCPServerConfig):
        self._config = config
        self._async = _AsyncMCPBridge(config)
        self._llm_tools: list[dict] = []
        self._loop: asyncio.AbstractEventLoop | None = None

    def _run(self, coro) -> Any:
        """Run an async coroutine in the bridge's dedicated event loop."""
        if self._loop is None or self._loop.is_closed():
            self._loop = asyncio.new_event_loop()
            asyncio.set_event_loop(self._loop)
        return self._loop.run_until_complete(coro)

    @property
    def server_name(self) -> str:
        return self._config.name

    @property
    def transport_type(self) -> str:
        return self._config.transport_type

    @property
    def llm_tools(self) -> list[dict]:
        return self._llm_tools

    def ensure_ready(self) -> bool:
        """Start / connect to the MCP server. Returns True if ready."""
        return self._run(self._async.ensure_ready())

    def load_tools(self) -> list[dict]:
        """Load tool definitions from the MCP server."""
        tools = self._run(self._async.list_tools())
        self._llm_tools = tools
        return tools

    def call_tool(self, tool_name: str, arguments: dict) -> dict:
        """Call a tool on the MCP server."""
        return self._run(self._async.call_tool(tool_name, arguments))

    def shutdown(self):
        """Shutdown the MCP server connection and close the event loop."""
        try:
            self._run(self._async.shutdown())
        except Exception:
            pass
        finally:
            if self._loop and not self._loop.is_closed():
                self._loop.close()
            self._loop = None
