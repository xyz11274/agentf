"""MCP 接入层 — MCP server 工具桥接。

与 harness/tools/（系统工具，四属性契约）分离，避免 MCP 基础设施
被 _load_dir 当作工具候选扫描。

公共 API：
  - MCP_AVAILABLE: mcp SDK 是否可用（模块级 import 不硬依赖 mcp）
  - MCPServerConfig / discover_servers: .mcp.json 配置发现
  - MCPBridge: 同步包装的 MCP server 连接与工具调用（ensure_ready/load_tools/call_tool/shutdown）
"""

from agentflow_runtime.harness.mcp.mcp_bridge import (
    MCP_AVAILABLE,
    MCPBridge,
    MCPServerConfig,
    discover_servers,
)

__all__ = ["MCP_AVAILABLE", "MCPBridge", "MCPServerConfig", "discover_servers"]
