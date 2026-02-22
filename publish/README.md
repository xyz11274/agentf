# AgentFlow Agent Publishing Guide

本文档面向业务开发团队，说明如何开发并发布一个新的业务 Agent 包。

## 1. Agent 包开发
参考 `publish/agent/AGENT_PACKAGE_SPEC.md`，按规范准备 Agent 包内容。

### 1.1 必备文件
- `agent.json`：Agent 名称、版本、工作流清单
- `agentCard.md`：Agent Prompt/能力说明
- `workflow_XX.json`：流程定义
- `node_xxx.$TYPE`：可执行部件（`.sh/.py/.so/.jar`）
- `package.json`：外层包描述与文件清单

### 1.2 打包
使用：

```bash
./publish/agent_build.sh build <agent_source_dir> [output_dir]
```

产物：
- 外层：`$xxxAgent_$PKGTYPE_$PKGVER.tar.gz`
- 内层：`$xxxAgent_$AGTVER.tar.gz`

## 2. Workflow 开发
Workflow 规范样例位于：
- `publish/workflow/workflow_sample.jsonc`

开发时重点定义：
- `nodes`：负责执行，明确执行模式与执行部件
- `edges`：负责控制流程与数据传递
- 统一控制字段：`edge.control.type + edge.control.*`，支持 `if/loop/fork/join/async/goto`
- 支持环回流程：通过 `control.type = "goto"` 或 `loop` 允许节点重入

可复用内置模板：
- `publish/workflow/memory`
- `publish/workflow/skills`
- `publish/workflow/tools`

固定流程模式：
- `publish/workflow/pattern/serial_workflow.json`
- `publish/workflow/pattern/parallel_workflow.json`
- `publish/workflow/pattern/cycled_workflow.json`

## 3. Node 定制开发
Node 规范文档：
- `publish/workflow/NODE_DEVELOPMENT_SPEC.md`

Node API 手册：
- `publish/workflow/NODE_API_REFERENCE.md`

固定 Node 模式模板：
- `publish/workflow/node_patterns/qa_node_template.md`
- `publish/workflow/node_patterns/api_tool_select_template.md`
- `publish/workflow/node_patterns/mcp_tool_select_template.md`
- `publish/workflow/node_patterns/skills_select_template.md`

建议实现统一入口（输入/输出 JSON、明确退出码），并在 workflow node 中声明 `execution_mode` 与 `command`。
