# Gap 04: MCP 协议集成缺失

## 状态
⚠️ **示例层已实现**（`examples/agent/harness-agent/harness/tools/mcp_bridge.py`），C++ 框架层仍无原生支持

## 问题描述

AgentFlow 的 C++ 框架层（`src/agent/`、`src/runtime/`）中**没有任何 MCP（Model Context Protocol）相关代码**。MCP Server 的调用只能通过用户在 workflow 的 shell/Python node 中手动实现。

## 代码搜索

```bash
grep -rn "mcp\|MCP" src/agent/ src/runtime/  # 返回空
```

## 当前情况

项目根目录有 `.mcp.json`（配置了 tavily-mcp），原先这个 MCP 是给 **AtomCode（AI 编码工具）** 用的。

### 已有实现（2026-06-22）

`harness-agent` 示例层已通过官方 `mcp` Python SDK 实现 MCP 集成：

| 文件 | 说明 |
|------|------|
| `examples/agent/harness-agent/harness/tools/mcp_bridge.py` | MCP Bridge，基于 Anthropic 官方 `mcp` SDK，支持 stdio/SSE/Streamable HTTP |
| `examples/agent/harness-agent/harness/tools/__init__.py` | 集成 MCP 工具发现和路由，与本地工具共存 |

验证结果：
- 通过 tavily-mcp 搜索返回 23,834 字符真实搜索结果
- 本地工具与 MCP 工具共存（7 个本地 + 5 个 MCP）
- 零 C++ 框架改动

## 影响（剩余）

- C++ 框架层仍无原生 MCP 支持，其他 agent package 需自行实现
- 需通过 `pip install mcp` 安装 SDK 依赖

## 建议方向

| 方案 | 说明 | 工作量 |
|------|------|--------|
| **内嵌 MCP Client** | C++ 中实现 MCP Client，作为内置 skill 节点 | 大 |
| **MCP Agent Node** | 创建一个通用的 Python MCP agent node，通过子进程调用 `npx -y @modelcontextprotocol/server-xxx` | 中 ✅ 已实现 |
| **Bridge 模式** | agent 通过 JSON-RPC 请求 daemon，daemon 转发给 MCP Server | 中 |

## 参考

- `.mcp.json` — 当前 MCP 配置
- `examples/agent/harness-agent/harness/tools/mcp_bridge.py` — 已实现的 MCP Bridge
- `docs/enhancements/040-feature-harness-agent-mcp-support-260622-tbd.md` — MCP 集成设计文档
- `publish/workflow/tools/workflow.json` — 内置 tools workflow 骨架
