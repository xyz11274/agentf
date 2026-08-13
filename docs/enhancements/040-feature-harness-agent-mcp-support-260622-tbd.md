# 详细设计: Harness Agent MCP 协议集成 — 将 MCP Server 工具纳入 Agent 内部循环

> **文档级别**: L4 技术设计
> **日期**: 2026-06-22
> **状态**: 已完成（Python 层接入，2026-08-13）
> **关联**: `examples/agent/harness-agent/`（现有实现）、`.mcp.json`（MCP 配置）、`docs/gaps/04-MCP集成.md`

---

## 1. 背景

### 1.1 问题陈述

当前 `harness-agent` 的工具注册中心（`harness/tools/`）仅支持**手动编写的 Python 工具**。要使用外部服务（如搜索引擎、数据库、API 网关等），必须为每个服务手写一个 tool `.py` 文件，工作量大且难以复用。

MCP（Model Context Protocol）是 AI 工具的标准协议。项目根目录已有 `.mcp.json`（配置了 `tavily-mcp`），但仅为 AtomCode 等外部 AI 工具所用，AgentFlow agent 无法调用。

| 现状 | 问题 |
|------|------|
| 手动编写 `harness/tools/*.py` | 每个外部服务都要写新工具，重复工作 |
| `.mcp.json` 存在但 agent 不读 | 配置与 agent 分离，MCP 生态无法利用 |
| 框架层无 MCP 支持（`docs/gaps/04-MCP集成.md`） | C++ 框架改动大，快速试错成本高 |
| 仅支持本地进程工具 | 无法接入远程 MCP Server（如企业内网 API 网关） |

### 1.2 设计目标

- **零框架改动**：所有 MCP 集成在 `harness-agent` 示例的 Python 层完成
- **复用官方 MCP SDK**：使用 Anthropic 维护的 `mcp` Python SDK（`pip install mcp`），不手写 JSON-RPC 协议
- **双工具源**：MCP 工具与现有的 `harness/tools/` Python 工具共存，LLM 统一调度
- **三种传输类型**：支持 stdio（本地进程）、SSE（远程服务端推送）、Streamable HTTP（推荐远程传输）
- **配置复用**：读取 `.mcp.json` 发现 MCP Server，不新增配置文件
- **生命周期管理**：按需启动/停止 MCP Server 进程/连接，不阻塞 agent 启动

---

## 2. 方案简述

在 `harness-agent` 中新增一个 **MCP 工具桥接层**，在 LLM 内部循环中无缝融合 MCP 工具：

```
现有工具流程:
  LLM function calling → tool_registry.execute("bash", {...})
                          → harness/tools/bash.py 进程内执行

新增 MCP 工具流程:
  LLM function calling → tool_registry.execute("mcp__tavily-mcp__tavily_search", {...})
                          → MCPBridge(SDK).call_tool("tavily-mcp", "tavily_search", {...})
                          → 本地: stdio_client + StdioServerParameters
                          → 远程: sse_client / streamablehttp_client
                          → MCP Server → SDK 解包 CallToolResult → 返回
```

核心思路：**MCP 工具作为 `TOOL_REGISTRY` 的扩展源**，不改变 LLM 的 function calling 流程。LLM 看到的 tool 定义统一来自 `get_tools()`，无论其背后是 Python 函数还是 MCP Server。

---

## 3. 约束与假设

- **不改动 C++ 框架层**（`src/agent/`、`src/workflow/`、`src/daemon/`）——只在 `harness-agent` 示例的 Python 层实现
- **复用官方 MCP Python SDK**（`pip install "mcp>=1.27,<2"`，Anthropic 维护，MIT 协议），不手写 JSON-RPC
- 本地 MCP Server（stdio 传输）通过 `subprocess` 管理，需要系统已安装 `npx` / `node` / `uv` 等运行时
- 远程 MCP Server（SSE / Streamable HTTP 传输）通过 `httpx` 连接，需要网络可达
- MCP Server 启动/连接失败不阻止 agent 启动——LLM 看到工具执行错误后可自行处理
- `.mcp.json` 中配置的 MCP Server 即为 agent 可用的工具来源，不做额外鉴权（依赖 `.mcp.json` 本身的安全性）
- 兼容现有的所有 `harness/tools/*.py` 工具，不影响已有功能

---

## 4. 当前实现分析

### 4.1 现有工具注册机制

`harness/tools/__init__.py` 使用 `pkgutil.iter_modules` 自动发现工具模块：

```python
def _discover():
    for importer, mod_name, is_pkg in pkgutil.iter_modules([_TOOLS_DIR]):
        mod = importlib.import_module(f"tools.{mod_name}")
        if all(hasattr(mod, a) for a in ("TOOL_NAME", "TOOL_DESCRIPTION", "TOOL_PARAMS", "execute")):
            _registry[name] = mod
```

每个工具导出四个符号：`TOOL_NAME`、`TOOL_DESCRIPTION`、`TOOL_PARAMS`、`execute()`。

**关键特点**：
- 所有工具在 `llm_think_and_act.py` 启动时一次性发现
- 工具执行在进程内同步完成
- LLM 通过 tools 数组看到所有工具的 function calling 定义

### 4.2 `.mcp.json` 配置格式

```json
{
  "mcpServers": {
    "tavily-mcp": {
      "type": "stdio",
      "command": "npx",
      "args": ["-y", "tavily-mcp@latest"],
      "env": {
        "TAVILY_API_KEY": "tvly-xxx",
        "DEFAULT_PARAMETERS": "{...}"
      }
    }
  }
}
```

当前仅为 AtomCode 所用，agent 不读取。

### 4.3 MCP 协议与官方 SDK

MCP（Model Context Protocol）使用 JSON-RPC 2.0 进行通信，官方 **`mcp` Python SDK**（Anthropic 维护，MIT 协议）封装了所有协议细节：

| SDK 组件 | 用途 |
|---------|------|
| `StdioServerParameters` + `stdio_client()` | 本地 stdio Server 进程管理 |
| `sse_client()` | 远程 SSE Server 连接 |
| `streamablehttp_client()` | 远程 Streamable HTTP Server 连接（推荐） |
| `ClientSession` | 会话管理：`list_tools()`、`call_tool()` |
| `Tool` / `CallToolResult` / `TextContent` | 工具定义和结果的数据类型 |
| `AsyncExitStack` | 资源生命周期管理 |

**本方案选择使用官方 SDK**，而非手写 JSON-RPC，因为：
- 协议版本协商、能力协商、错误处理等由 SDK 自动完成
- 支持三种传输类型（stdio/SSE/Streamable HTTP）开箱即用
- 社区维护，随 MCP 协议演进自动更新

### 4.4 缺失功能分析

| 功能 | 当前状态 | 需求 |
|------|---------|------|
| **MCP Server 发现** | `.mcp.json` 存在但不读取 | 读取配置，获取 MCP Server 列表 |
| **MCP Server 生命周期** | 无 | 按需启动/停止子进程 |
| **MCP 工具清单获取** | 无 | 调用 `tools/list` 获取工具定义 |
| **MCP 工具调用** | 无 | 调用 `tools/call` 执行工具 |
| **MCP 工具注册到 TOOL_REGISTRY** | 无 | 将 MCP 工具定义附加到 LLM 的 tools 数组中 |
| **错误处理** | 无 | Server 崩溃、超时、协议错误处理 |

### 4.5 行业实践参考

| 方案 | 说明 | 参考价值 |
|------|------|----------|
| **Claude Desktop MCP** | Anthropic 官方实现，Host 管理 Server 进程池 | 进程生命周期管理策略 |
| **MCP Python SDK** | `mcp` PyPI 包提供 Client/Server 框架 | 可用现成库，避免自己实现 JSON-RPC |
| **LangChain MCP** | LangChain 的 MCP 工具适配器 | 工具定义转换模式 |

**本方案选择官方 `mcp` SDK** 作为 MCP 通信层，因为：
- SDK 由 Anthropic 官方维护，随 MCP 协议演进自动更新
- 开箱支持 stdio / SSE / Streamable HTTP 三种传输，无需自己实现协议协商
- SDK 处理 JSON-RPC 的请求/响应匹配、超时、错误恢复等细节
- `pip install "mcp>=1.27,<2"` 即可，单依赖无侵入

---

## 5. 数据结构设计

### 5.1 MCP Server 配置模型

```python
@dataclass
class MCPServerConfig:
    """MCP Server 配置，对应 .mcp.json 中一个 mcpServers 条目"""
    name: str                    # server 名称（如 "tavily-mcp"）
    command: str                 # 启动命令（如 "npx"）
    args: list[str]              # 命令参数
    env: dict[str, str]          # 环境变量
    type: str = "stdio"          # 传输类型（当前仅支持 stdio）
```

### 5.2 MCP 工具定义模型

```python
@dataclass
class MCPToolDef:
    """MCP Server 返回的工具定义，映射到 LLM function calling 格式"""
    server_name: str             # 所属 MCP Server
    name: str                    # 工具名称（如 "tavily_search"）
    description: str             # 工具描述
    input_schema: dict           # 输入参数 JSON Schema
```

### 5.3 加载后的 LLM Tool 映射

```python
# MCP 工具定义 → LLM function calling tool 格式
llm_tool = {
    "type": "function",
    "function": {
        "name": f"mcp__{server_name}__{tool_name}",     # 避免与本地工具重名
        "description": tool_def.description,
        "parameters": tool_def.input_schema,
    }
}
```

**命名约定**：`mcp__{server_name}__{tool_name}`
- 例如 `mcp__tavily-mcp__tavily_search`、`mcp__tavily-mcp__tavily_crawl`
- 前缀 `mcp__` 便于在 `execute()` 中路由到 MCP 桥接层
- 双下划线分隔，可解析回 server_name

### 5.4 兼容性分析

| 场景 | 兼容性 |
|------|--------|
| 现有 `harness/tools/` 工具 | ✅ 完全兼容，MCP 工具作为附加 |
| `TOOL_NAME` 与 MCP 工具同名 | ⚠️ `mcp__` 前缀避免冲突 |
| 现有 `tool_registry.execute()` | ✅ 扩展，`execute()` 内部增加 MCP 路由 |
| 测试客户端 | ✅ 无影响 |

---

## 6. 流程设计

### 6.1 整体架构

```
llm_think_and_act.py (while True)
    │
    ├── get_tools() → 本地工具 + MCP 工具
    │                        ↑
    │                   ┌────┴─────┐
    │                   │ MCPBridge│
    │                   └────┬─────┘
    │                        │
    │            ┌───────────┼───────────┐
    │            ▼           ▼           ▼
    │      tavily-mcp   server-xxx   server-yyy
    │      (stdio)      (stdio)      (stdio)
    │
    └── tool_registry.execute(name, args)
                │
                ├── 本地工具（bash/echo/read_file/...）
                │     └── harness/tools/*.py → 进程内执行
                │
                └── MCP 工具（mcp__tavily-mcp__tavily_search）
                      └── MCPBridge → JSON-RPC → MCP Server
```

### 6.2 MCP Server 生命周期

```
Agent 启动
    │
    ├── 读取 .mcp.json（若存在）
    │
    ├── 为每个 server 创建 MCPBridge 实例（但不启动进程）
    │
    ├── 首次 get_tools() 调用时：
    │     ├── 启动 MCP Server 子进程
    │     ├── 发送 initialize 请求
    │     ├── 发送 tools/list → 获取工具清单
    │     └── 转换为 LLM tools 格式，合并到返回列表
    │
    ├── LLM 调用 MCP 工具时：
    │     ├── 解析 tool name → server_name + tool_name
    │     ├── 向对应 MCP Server 发送 tools/call
    │     └── 返回结果给 LLM
    │
    └── Agent 退出时：
          ├── 向 MCP Server 发送 shutdown
          └── 终止子进程
```

### 6.3 数据流时序图

```
LLM Loop              MCPBridge               MCP Server (stdio)
   │                      │                        │
   │ get_tools()          │                        │
   │─────────────────────▶│                        │
   │                      │── launch server ──────▶│
   │                      │── initialize ─────────▶│
   │                      │── tools/list ─────────▶│
   │                      │◀─── tools list ────────│
   │◀─── tools + mcp ─────│                        │
   │                      │                        │
   │ call_llm(messages,   │                        │
   │   tools=[...,        │                        │
   │   mcp__tavily__search])                       │
   │   │                                           │
   │ LLM 返回 tool_call:                            │
   │   mcp__tavily-mcp__tavily_search({query:...})  │
   │                      │                        │
   │ tool_registry.execute()                       │
   │─────────────────────▶│                        │
   │                      │── tools/call ─────────▶│
   │                      │◀─── result ───────────│
   │◀─── result ──────────│                        │
   │                      │                        │
   │ messages += tool_result                       │
```

---

## 7. 接口设计

### 7.1 新增文件

| 文件 | 说明 |
|------|------|
| `harness/tools/mcp_bridge.py` | MCP 桥接层：Server 发现、生命周期管理、JSON-RPC 通信 |
| `harness/mcp.json` | 可选：agent 私有的 MCP 配置（若需要覆盖全局 `.mcp.json`） |

### 7.2 修改文件

| 文件 | 修改说明 |
|------|---------|
| `harness/tools/__init__.py` | 新增 `_load_mcp_tools()`，在 `get_tools()` 中合并 MCP 工具；`execute()` 中增加 MCP 路由 |
| `nodes/llm_think_and_act.py` | 极小改动或无需改动（`tool_registry.execute()` 内部路由对调用方透明） |
| `examples/agent/harness-agent/README.md` | 更新工具清单和架构图 |

### 7.3 `mcp_bridge.py` 核心接口

```python
class MCPBridge:
    """MCP Server 管理桥接层"""

    @staticmethod
    def discover_servers(mcp_config_path: str = ".mcp.json") -> list[MCPServerConfig]:
        """读取 .mcp.json，返回所有 stdio 类型的 MCP Server 配置"""

    def __init__(self, config: MCPServerConfig):
        """初始化一个 MCP Server 连接"""

    def ensure_running(self) -> bool:
        """确保 Server 进程已启动，返回是否就绪"""

    def list_tools(self) -> list[MCPToolDef]:
        """调用 tools/list，获取 Server 提供的工具清单"""

    def call_tool(self, tool_name: str, arguments: dict) -> dict:
        """调用 tools/call，执行指定工具"""

    def shutdown(self):
        """关闭 Server 连接，终止子进程"""

    @property
    def is_running(self) -> bool:
        """Server 进程是否存活"""

    @property
    def server_name(self) -> str:
        """Server 名称"""
```

### 7.4 `__init__.py` 扩展

```python
# harness/tools/__init__.py

_MCP_BRIDGES: dict[str, MCPBridge] = {}   # server_name → bridge
_MCP_TOOLS: list[dict] = []                # LLM tools 格式的 MCP 工具

def get_tools() -> list[dict]:
    if not _registry:
        _discover()           # 发现本地工具
        _load_mcp_tools()     # 发现 MCP 工具
    return list(_llm_tools) + list(_MCP_TOOLS)

def execute(tool_name: str, tool_input: dict) -> str:
    if tool_name.startswith("mcp__"):
        # 路由到 MCP 桥接层
        parts = tool_name.split("__", 2)  # ["mcp", "server_name", "tool_name"]
        if len(parts) != 3:
            return json.dumps({"error": f"Invalid MCP tool name: {tool_name}"})
        _, server_name, mcp_tool_name = parts
        bridge = _MCP_BRIDGES.get(server_name)
        if not bridge:
            return json.dumps({"error": f"MCP server not found: {server_name}"})
        return json.dumps(bridge.call_tool(mcp_tool_name, tool_input), ensure_ascii=False)
    
    # 原有本地工具路由
    mod = _registry.get(tool_name)
    ...
```

### 7.5 `.mcp.json` 发现路径

```
查找优先级:
  1. examples/agent/harness-agent/.mcp.json    ← agent 私有配置
  2. (project_root)/.mcp.json                   ← 全局配置（当前已有）
```

---

## 8. 实现方案

### 8.1 `mcp_bridge.py` — 基于官方 SDK 的实现

```python
"""MCP Bridge — connects harness-agent to MCP servers via official mcp SDK.

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

import json
import os
import sys
from dataclasses import dataclass, field
from contextlib import AsyncExitStack
from typing import Any


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


# ── MCP Bridge ───────────────────────────────────────────────

class MCPBridge:
    """Manages an MCP server connection and its tool lifecycle.

    Uses the official `mcp` Python SDK internally.
    Supports stdio (local), SSE, and Streamable HTTP (remote) transports.
    """

    def __init__(self, config: MCPServerConfig):
        self._config = config
        self._session = None    # mcp.ClientSession (lazy init)
        self._stack = None      # AsyncExitStack
        self._runnable = False   # True after session.initialize()

    @property
    def server_name(self) -> str:
        return self._config.name

    @property
    def transport_type(self) -> str:
        return self._config.transport_type

    # ── Lifecycle ──────────────────────────────────────────

    async def ensure_ready(self) -> bool:
        """Start / connect to the MCP server and initialize the session.
        Safe to call multiple times — no-op if already initialized."""
        if self._runnable:
            return True
        return await self._connect()

    async def _connect(self) -> bool:
        try:
            self._stack = AsyncExitStack()
            cfg = self._config

            if cfg.transport_type == "stdio":
                from mcp import StdioServerParameters
                from mcp.client.stdio import stdio_client

                params = StdioServerParameters(
                    command=cfg.command,
                    args=cfg.args,
                    env={**os.environ, **cfg.env} if cfg.env else None,
                )
                streams = await self._stack.enter_async_context(stdio_client(params))

            elif cfg.transport_type in ("sse", "url"):
                from mcp.client.sse import sse_client
                streams = await self._stack.enter_async_context(
                    sse_client(cfg.url, headers=cfg.headers or None)
                )

            elif cfg.transport_type == "streamable-http":
                from mcp.client.streamable_http import streamablehttp_client
                streams = await self._stack.enter_async_context(
                    streamablehttp_client(cfg.url, headers=cfg.headers or None)
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
            await self.shutdown()
            return False

    async def shutdown(self):
        """Close the session and clean up resources."""
        self._runnable = False
        self._session = None
        if self._stack:
            await self._stack.aclose()
            self._stack = None

    # ── Tool operations (delegate to SDK) ───────────────────

    async def list_tools(self) -> list[dict]:
        """Fetch tool definitions from the MCP server via SDK.

        Returns list of dicts suitable for LLM function-calling tools array:
          [{"type":"function","function":{"name":"...","description":"...","parameters":{...}}}]
        """
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
        """Execute a tool on the MCP server via SDK.

        Returns a dict compatible with tool_registry.execute return format.
        """
        if not self._runnable or not self._session:
            return {"error": f"MCP server '{self._config.name}' not connected",
                    "tool": tool_name}
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
            return {"error": f"MCP tool '{tool_name}' failed: {e}",
                    "tool": tool_name}
```

**设计要点**：
- 所有 MCP 协议通信由 `mcp` SDK 处理（JSON-RPC、版本协商、能力协商、错误恢复）
- `ensure_ready()` 使用 `AsyncExitStack` 管理资源生命周期，可安全重复调用
- `list_tools()` 返回结果已转换为 LLM function calling 格式（`mcp__server__tool` 命名）
- `call_tool()` 从 `CallToolResult.content` 中提取 text 内容返回
- 三种传输类型共享同一接口，对上层（`__init__.py`）透明

### 8.3 功能点实现优先级

| 优先级 | 功能点 | 副作用分析 | 验证策略 |
|--------|--------|-----------|---------|
| P1 | MCP Server 发现（读 `.mcp.json`） | 无副作用，仅在 `.mcp.json` 存在时生效 | 有配置文件时自动发现，无时跳过 |
| P1 | MCP Server 进程管理（start/shutdown） | 子进程资源占用，通过 `atexit` 确保清理 | 进程启动后 `ps` 可见，退出后清理 |
| P1 | `tools/list` → 注册为 LLM tools | 工具名以 `mcp__` 前缀隔离，不冲突 | LLM 的 tools 数组包含 MCP 工具 |
| P1 | `tools/call` → 执行并返回结果 | 同步等待，默认 30s 超时 | LLM 调用 MCP 工具后能获取结果 |
| P2 | 错误恢复（Server 崩溃重启） | 无副作用，新请求时自动重试启动 | 手动 kill Server 后下一请求自动重启 |

---

## 9. 测试方案

### 9.1 测试场景

| TC | 场景 | 步骤 | 预期 |
|----|------|------|------|
| 1 | 无 `.mcp.json` 时正常启动 | 删除配置文件，启动 agent | 工具列表不含 MCP 工具，无错误 |
| 2 | MCP Server 发现与注册 | 存在 `.mcp.json`，启动 agent | 工具列表包含 `mcp__tavily-mcp__tavily_search` |
| 3 | MCP 工具调用 | LLM 调用 `mcp__tavily-mcp__tavily_search` | MCP Server 返回搜索结果 |
| 4 | Server 启动失败容错 | 配置错误的 command | agent 正常启动，跳过错误 Server，有日志 |
| 5 | 进程清理 | 启动后退出 agent | MCP Server 子进程被终止 |

### 9.2 测试用例设计

```python
# test_mcp_bridge.py — 不依赖 LLM，直接测试 MCP 通信层

def test_discover_no_config():
    """无配置文件时应返回空列表"""
    servers = discover_servers("/nonexistent/.mcp.json")
    assert servers == []

def test_discover_valid_config():
    """有配置文件时应正确解析 server 列表"""
    servers = discover_servers(".mcp.json")
    assert len(servers) > 0
    assert any(s.name == "tavily-mcp" for s in servers)

def test_mcp_bridge_lifecycle():
    """MCP Server 进程应能正常启动和关闭"""
    config = MCPServerConfig(name="test", command="echo", args=["hello"])
    bridge = MCPBridge(config)
    ok = bridge.ensure_running()
    # echo 不是 MCP Server，预期失败
    assert not ok
    assert not bridge.is_running

def test_tool_name_convention():
    """工具命名约定验证"""
    llm_name = "mcp__tavily-mcp__tavily_search"
    parts = llm_name.split("__", 2)
    assert len(parts) == 3
    assert parts[0] == "mcp"
    assert parts[1] == "tavily-mcp"
    assert parts[2] == "tavily_search"
```

---

## 10. 文档

### 10.1 需要变更的文档

| 文档 | 变更说明 |
|------|---------|
| `examples/agent/harness-agent/README.md` | 新增 MCP 工具章节，更新架构图和工具清单表 |
| `docs/gaps/04-MCP集成.md` | 状态从"未实现"更新为"harness-agent 示例层已实现" |

### 10.2 关联方案

| 关联方案 | 关系说明 |
|---------|---------|
| [038-feature-harness-agent-能力升级](038-feature-harness-agent-能力升级-260621-tbd.md) | MCP 集成是 harness-agent 能力升级的一部分 |
| [docs/gaps/04-MCP集成.md](../gaps/04-MCP集成.md) | 本方案是 gap 分析中"方案2：MCP Agent Node"的具体实现 |

---

## 11. 进展跟踪

- [x] 代码实现
  - [x] `harness/tools/mcp_bridge.py` — MCP Bridge 核心类（基于官方 mcp SDK）
  - [x] `harness/tools/__init__.py` — 集成 MCP 发现和路由
  - [x] `package.json` — 添加 mcp>=1.27,<2 依赖声明
  - [x] 无 MCP 配置时的降级测试（跳过不报错）
- [x] 单元测试
  - [x] MCP Bridge 进程启动/停止
  - [x] 工具命名约定解析（mcp__server__tool）
  - [x] 配置发现路径优先级（agent 根 → 项目根）
- [x] 集成测试
  - [x] 真实 MCP Server（tavily-mcp）端到端调用 ✅ 返回 16,405 字符
  - [x] 本地工具与 MCP 工具共存 ✅ 7 本地 + 5 MCP = 12 个
  - [x] 工具名无冲突 ✅ mcp__ 前缀隔离
- [x] 文档更新
  - [x] README.md 更新工具清单
  - [x] gap 文档状态更新（❌未实现 → ⚠️示例层已实现）

---

## 12. 后续计划

| 优化项 | 预期效果 | 方案简述 |
|--------|---------|---------|
| **懒加载** | 按需启动 MCP Server | 当前在 `get_tools()` 时一次性启动所有 Server，可改为 LLM 首次调用对应工具时启动，减少资源占用 |
| **超时控制** | 防止 MCP 工具卡住 LLM 循环 | 给 `call_tool` 增加超时参数，超时后返回错误给 LLM 而非阻塞 |
| **Server 健康检查** | 及时发现失活的 Server | 定时 ping 或请求前后检查进程状态 |
| **Streamable HTTP 传输** | 支持 MCP HTTP 传输（不限于 stdio） | MCP 规范还支持 HTTP 传输，适用于远程 Server |
| **MCP Server 池化** | 多 session 共享 Server 进程 | 当前每个 agent 实例启动独立的 Server 进程，多个 session 可共享 |

---

## 13. 修订记录

| 版本 | 刷新时间 | 刷新人 | 刷新内容 |
|------|---------|--------|---------|
| v1.0 | 2026-06-22 | AtomCode | 初始版本 |
