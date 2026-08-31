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
import logging
import os
import urllib.parse
from contextlib import AsyncExitStack
from dataclasses import dataclass, field
from typing import Any

from agentflow_runtime.security import (
    find_secret_exposures,
    find_registered_secret_exposures,
    redact_secrets,
    register_environment_secrets,
    register_secret,
    safe_subprocess_environment,
)

# mcp SDK 可用性标志（I-7: 模块级 import 不硬依赖 mcp）
try:
    import mcp  # noqa: F401
    MCP_AVAILABLE = True
except ImportError:
    MCP_AVAILABLE = False

logger = logging.getLogger(__name__)


# httpx client factory for MCP connections.  Certificate verification stays
# enabled so authentication headers cannot silently cross an untrusted TLS
# connection.  Internal CAs can be configured through SSL_CERT_FILE.
def _unverified_http_client(
    headers: dict[str, str] | None = None,
    timeout: Any = None,
    auth: Any = None,
) -> Any:
    """Create an httpx.AsyncClient with certificate verification enabled."""
    try:
        import httpx
    except ImportError:
        raise ImportError("httpx is required for MCP remote connections")

    kwargs: dict[str, Any] = {
        "verify": True,
        "follow_redirects": False,
    }
    if headers is not None:
        kwargs["headers"] = headers
    if timeout is not None:
        kwargs["timeout"] = timeout
    if auth is not None:
        kwargs["auth"] = auth
    return httpx.AsyncClient(**kwargs)


def _redact_arguments(arguments: dict) -> str:
    """Serialize arguments after credential and image payload redaction."""

    def _redact_value(v):
        if isinstance(v, str) and v.startswith("data:image/"):
            return f"<base64 len={len(v)}>"
        if isinstance(v, dict):
            return {k: _redact_value(vv) for k, vv in v.items()}
        if isinstance(v, list):
            return [_redact_value(vv) for vv in v]
        return v

    try:
        safe_arguments, _report = redact_secrets(arguments)
        return json.dumps(_redact_value(safe_arguments), ensure_ascii=False)
    except Exception as exc:
        safe_error, _report = redact_secrets(str(exc))
        return json.dumps({"redaction_error": safe_error}, ensure_ascii=False)


def _register_config_credentials(config: "MCPServerConfig") -> None:
    """Register credentials explicitly entrusted to one MCP server."""
    register_environment_secrets(config.env)
    for name, value in config.headers.items():
        normalized = str(name).lower()
        if any(marker in normalized for marker in (
            "authorization", "cookie", "token", "key", "secret", "credential",
        )):
            register_secret(f"mcp.{config.name}.header.{name}", str(value))

    previous = ""
    for index, value in enumerate(config.args):
        normalized = previous.lower().lstrip("-")
        if any(marker in normalized for marker in (
            "token", "key", "secret", "password", "credential",
        )):
            register_secret(f"mcp.{config.name}.arg.{index}", str(value))
        previous = str(value)


def _validate_remote_transport(config: "MCPServerConfig") -> None:
    """Require authenticated remote transport and reject credentials in URLs."""
    parsed = urllib.parse.urlsplit(config.url)
    if parsed.username is not None or parsed.password is not None:
        raise ValueError("MCP endpoint credentials must be supplied as headers")
    if find_secret_exposures(config.url).applied:
        raise ValueError("MCP endpoint URL must not contain credentials")
    if parsed.scheme == "https" and parsed.hostname:
        return
    if parsed.scheme == "http" and parsed.hostname in {"127.0.0.1", "::1", "localhost"}:
        return
    raise ValueError("remote MCP endpoint must use HTTPS")


# Configuration Model

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


# Server Discovery

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
            if not isinstance(cfg, dict):
                # D1: 畸形配置（null/非 dict）只跳过该 server，不拖垮其他 server
                logger.warning("mcp.discover.skip server=%s invalid_config", name)
                continue
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


# Async MCP Bridge (internal)

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
        cfg = self._config
        try:
            register_environment_secrets()
            _register_config_credentials(cfg)
            self._stack = AsyncExitStack()

            if cfg.transport_type == "stdio":
                from mcp.client.stdio import stdio_client
                from mcp import StdioServerParameters

                child_env = safe_subprocess_environment()
                child_env.update({str(key): str(value) for key, value in cfg.env.items()})
                params = StdioServerParameters(
                    command=cfg.command,
                    args=cfg.args,
                    env=child_env,
                )
                streams = await self._stack.enter_async_context(stdio_client(params))

            elif cfg.transport_type in ("sse", "url"):
                from mcp.client.sse import sse_client
                _validate_remote_transport(cfg)
                streams = await self._stack.enter_async_context(
                    sse_client(cfg.url, headers=cfg.headers or None,
                               httpx_client_factory=_unverified_http_client)
                )

            elif cfg.transport_type == "streamable-http":
                from mcp.client.streamable_http import streamablehttp_client
                _validate_remote_transport(cfg)
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
            # O-3: 连接成功日志
            safe_url, _report = redact_secrets(cfg.url)
            safe_command, _report = redact_secrets(cfg.command)
            logger.info(
                "mcp.connect.ok server=%s transport=%s url=%s command=%s",
                cfg.name, cfg.transport_type, safe_url, safe_command)
            return True

        except Exception as e:
            # O-3: 连接失败日志（含异常全文）
            safe_url, _report = redact_secrets(cfg.url)
            safe_command, _report = redact_secrets(cfg.command)
            safe_error, _report = redact_secrets(str(e))
            logger.warning(
                "mcp.connect.fail server=%s transport=%s url=%s command=%s error=%s",
                cfg.name, cfg.transport_type, safe_url, safe_command, safe_error)
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
            # O-4: list_tools 发现成功日志
            logger.info(
                "mcp.list_tools.ok server=%s count=%d",
                self._config.name, len(tools))
            safe_tools, _report = redact_secrets(tools)
            return safe_tools
        except Exception as e:
            # O-4: list_tools 失败日志
            safe_error, _report = redact_secrets(str(e))
            logger.warning(
                "mcp.list_tools.fail server=%s error=%s",
                self._config.name, safe_error)
            return []

    async def call_tool(self, tool_name: str, arguments: dict) -> dict:
        if not self._runnable or not self._session:
            return {"error": f"MCP server '{self._config.name}' not connected"}
        exposure = find_registered_secret_exposures(arguments)
        if exposure.applied:
            return {
                "error": "MCP tool call blocked by credential isolation policy",
                "status": "security_policy_blocked",
                "security": exposure.to_dict(),
                "tool": tool_name,
            }
        try:
            # O-1: 请求前日志（arguments 完整 json.dumps，base64 字段脱敏）
            logger.info(
                "mcp.call_tool.request server=%s tool=%s arguments=%s",
                self._config.name, tool_name, _redact_arguments(arguments))
            result = await self._session.call_tool(tool_name, arguments)
            texts = []
            for content in (result.content or []):
                if hasattr(content, 'text'):
                    texts.append(content.text)
                elif hasattr(content, 'type') and content.type == 'text':
                    texts.append(content.text or '')
            joined, result_security = redact_secrets("\n".join(texts))
            is_err = getattr(result, 'isError', False)
            err_text = ""
            if is_err and hasattr(result, 'content'):
                # 尝试从 content 提取错误文本
                for content in (result.content or []):
                    if hasattr(content, 'text') and content.text:
                        err_text, _report = redact_secrets(content.text)
                        break
            # O-2: 响应后日志（tool_name、result 字符长度/预览、isError、error 全文）
            # result_preview 为 F7 preview 模式：截断视图（超 200 字符加 ... 标记），完整数据在 result 返回值中
            result_preview = (joined[:200] + "...") if len(joined) > 200 else joined
            logger.info(
                "mcp.call_tool.response tool=%s result_len=%d isError=%s result_preview=%s error=%s",
                tool_name, len(joined), is_err, result_preview, err_text)
            return {
                "result": joined,
                "isError": is_err,
                "tool": tool_name,
                "security": result_security.to_dict(),
            }
        except Exception as e:
            safe_error, _report = redact_secrets(str(e))
            logger.warning(
                "mcp.call_tool.fail server=%s tool=%s error=%s",
                self._config.name, tool_name, safe_error)
            return {"error": f"MCP tool '{tool_name}' failed: {safe_error}"}


# Sync MCP Bridge (public API)

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
        # Q1: set_event_loop 有线程级全局副作用；安全前提是 skill_executor 以独立进程同步执行。
        # 若未来在 asyncio 上下文直接调用 MCPBridge，须改用 run_coroutine_threadsafe 或复用外部 loop。
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
        except Exception as e:
            # I1: 清理路径异常也记录日志（M3），不静默吞没
            safe_error, _report = redact_secrets(str(e))
            logger.warning("mcp.shutdown.fail server=%s error=%s", self._config.name, safe_error)
        finally:
            if self._loop and not self._loop.is_closed():
                self._loop.close()
            self._loop = None
