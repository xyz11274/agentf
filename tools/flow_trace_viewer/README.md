# 调度流追踪可视化工具（Flow Trace Viewer）

基于浏览器的可视化工具，用于查看 AgentFlow 调度流追踪日志（FlowTrace 系统）。

## 功能特性

- **时序图**：以 Mermaid 时序图展示组件间消息流转时序
- **流程图**：展示各实体间的全局调度流向及消息频次/耗时汇总
- **事件明细表**：完整事件列表，含时间戳、来源、目标、耗时、输入/输出、扩展属性
- **会话/对话过滤**：按 Session 和 Dialogue 维度筛选事件
- **拖拽加载**：直接将日志文件拖入浏览器即可加载
- **示例数据**：内置示例，一键加载演示

## 使用方法

1. 在现代浏览器（Chrome / Firefox / Safari / Edge 90+）中打开 `index.html`
2. 选择以下任一方式加载数据：
   - 将 `.ndjson` / `.log` 文件拖入拖放区
   - 点击"选择文件"手动选取
   - 点击"加载示例数据"查看演示

## 日志格式

本工具读取 JSON Lines（NDJSON）格式，每行一个 JSON 对象：

```json
{"time":"2026-03-13 18:23:55.660","type":"task-exec","session":"session-001","dialogue":"12345678","from":"external","to":"dispatcher","event":"task-created","inputs":"{\"user_text\":\"帮我订一张去北京的机票\"}"}
{"time":"2026-03-13 18:23:55.670","type":"node-exec","session":"session-001","dialogue":"12345678","from":"skill-selection-workflow","to":"select_skills","event":"node-completed","duration_us":7000,"outputs":"{\"next_skills\":[\"travel\"]}"}
```

### 必填字段

| 字段 | 说明 |
|------|------|
| `time` | 时间戳，格式 `YYYY-MM-DD HH:MM:SS.mmm` |
| `type` | 事件分类：`task-exec` / `workflow-exec` / `node-exec` / `skill-exec` / `external-call` |
| `from` | 来源实体名称 |
| `to` | 目标实体名称 |
| `event` | 事件类型名称 |

### 可选字段

| 字段 | 说明 |
|------|------|
| `session` | Session ID，用于会话级过滤 |
| `dialogue` | Dialogue ID，用于对话级过滤 |
| `inputs` | 输入数据（JSON 字符串或任意字符串） |
| `outputs` | 输出数据（JSON 字符串或任意字符串） |
| `duration_us` | 耗时（微秒） |
| `task_id` | 关联的 TraceTask ID（可选） |
| `trace_id` | 关联的全链路 trace_id（可选） |
| `attrs` | 扩展键值对，格式 `{"key": "value"}` |

### 事件类型说明

| 事件名 | 说明 |
|--------|------|
| `task-created` | 外部请求创建新任务 |
| `task-dispatched` | 任务分发给 Dispatcher |
| `workflow-scheduled` | Workflow 调度完成 |
| `workflow-started` | Workflow 开始执行 |
| `workflow-completed` | Workflow 执行完成 |
| `node-started` | Node 开始执行 |
| `node-completed` | Node 执行完成 |
| `skill-launched` | Skill 任务启动 |
| `skill-completed` | Skill 执行完成 |
| `data-exchange` | 组件间数据交换 |
| `dialogue-created` | 新对话创建 |
| `dialogue-completed` | 对话结束 |
| `external-call` | 外部回调调用 |
| `response-sent` | 响应发送给外部 |

## 生成追踪日志

### 方式一：环境变量（文件输出）

```bash
export FLOW_TRACE_FILE=/path/to/trace.ndjson
./your_agent_program
```

### 方式二：C++ API

```cpp
#include "runtime/flow_trace.h"

// 启用文件输出
agentflow::runtime::FlowTraceWriter::instance().set_output_file("/path/to/trace.ndjson");

// 关闭标准输出（默认开启）
agentflow::runtime::FlowTraceWriter::instance().set_stdout_enabled(false);

// 禁用追踪
agentflow::runtime::FlowTraceWriter::instance().set_enabled(false);
```

### 代码打点宏（C++ 内部使用）

```cpp
// 任务级事件
FLOW_TRACE_TASK(session_id, dialogue_id, from, to, FlowEventType::TASK_CREATED);

// Workflow 级事件（含输入输出和耗时）
FLOW_TRACE_WORKFLOW_WITH_DATA(session_id, dialogue_id, from, to,
    FlowEventType::WORKFLOW_COMPLETED, inputs_json, outputs_json, duration_us);

// Node 级事件（含输入输出和耗时）
FLOW_TRACE_NODE_WITH_DATA(session_id, dialogue_id, from, to,
    FlowEventType::NODE_COMPLETED, inputs_json, outputs_json, duration_us);

// 外部回调事件
FLOW_TRACE_EXTERNAL_CALL(session_id, dialogue_id, from, to,
    "enter-callback", in_data, out_data);
```

## 与 Span Trace Viewer 的区别

| 维度 | Flow Trace Viewer（本工具）| Span Trace Viewer |
|------|--------------------------|-------------------|
| 数据来源 | `FlowTraceWriter` | `TraceManager` |
| 关注点 | 调度流、消息流转 | 性能、调用链耗时 |
| 可视化 | 时序图 + 流程图 | 甘特图 + 火焰图 |
| 日志字段 | `time`, `from`, `to`, `event` | `trace_id`, `span_id`, `kind` |

如需可视化 Span 性能跟踪数据，请使用 `tools/span_trace_viewer/`。

## 依赖

- **Mermaid.js**（CDN 加载）：用于时序图和流程图渲染
- 无构建步骤，直接在浏览器中打开即用
