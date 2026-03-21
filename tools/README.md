# AgentFlow 开发工具集（tools/）

本目录包含 AgentFlow 框架配套的可视化与调试工具，均为零依赖的浏览器端静态工具，无需构建步骤。

## 工具一览

### 🔍 flow_trace_viewer — 调度流追踪可视化

**路径**：`tools/flow_trace_viewer/index.html`

可视化 **FlowTrace 调度流**日志，展示 Agent 内部消息流转的时序与路径。

| 视图 | 说明 |
|------|------|
| 时序图 | Mermaid 时序图，展示组件间消息调用顺序 |
| 流程图 | 实体间流向及消息频次汇总 |
| 事件明细表 | 完整事件列表，含输入/输出/扩展属性 |

**数据来源**：`FlowTraceWriter`（`include/internal/runtime/flow_trace.h`）

```bash
# 通过环境变量启用文件输出
export FLOW_TRACE_FILE=/path/to/trace.ndjson
./your_agent_program

# 然后在浏览器中打开
open tools/flow_trace_viewer/index.html
```

详细说明见 [`flow_trace_viewer/README.md`](flow_trace_viewer/README.md)

---

### 📊 span_trace_viewer — 全链路 Span 跟踪可视化

**路径**：`tools/span_trace_viewer/index.html`

可视化 **TraceManager 全链路跟踪**数据，展示 Span 的性能时序与调用树。

| 视图 | 说明 |
|------|------|
| 时序甘特图 | 按线程分组，X 轴为绝对时间，颜色区分 Span 类型 |
| 火焰图 | 按 parent_span_id 构建调用树，宽度正比于耗时 |
| Span 列表 | 完整字段表格，支持 tags/events 展示 |
| 统计指标 | 加载 metrics.json，查看 p50/p90/p95/p99 分位数 |

**数据来源**：`TraceManager`（`include/internal/runtime/trace/trace_manager.h`）

```cpp
// C++ API 示例
#include "runtime/trace/trace_manager.h"
using namespace agentflow::runtime::trace;

TraceManager::instance().set_output_dir("trace_output");
std::string task_id = TraceManager::instance().create_task("my-task", filter);
TraceManager::instance().start_task(task_id);
// ... 运行 Agent ...
TraceManager::instance().stop_task(task_id);
// 输出：trace_output/task_<task_id>.ndjson

// 打开可视化工具
// open tools/span_trace_viewer/index.html
```

详细说明见 [`span_trace_viewer/README.md`](span_trace_viewer/README.md)

---

## 工具对比

| 特性 | flow_trace_viewer | span_trace_viewer |
|------|-------------------|-------------------|
| 关注点 | 调度流、消息流转 | 执行性能、调用链耗时 |
| 数据来源 | `FlowTraceWriter` | `TraceManager` |
| 日志字段 | `time`, `from`, `to`, `event`, `session`, `dialogue` | `trace_id`, `span_id`, `kind`, `start_us`, `duration_us` |
| 可视化形式 | 时序图 + 流程图 | 甘特图 + 火焰图 |
| 特有功能 | 对话隔离过滤 | Metrics 统计视图 |
| 依赖 | Mermaid.js (CDN) | 无外部依赖 |

**使用建议**：
- 排查消息流转路径或定位某条请求"卡在哪"→ 用 `flow_trace_viewer`
- 性能分析、找慢节点、调用树分析 → 用 `span_trace_viewer`
- 两者可以通过 `task_id` / `trace_id` 字段进行跨系统关联

## 快速启动

```bash
# macOS
open tools/flow_trace_viewer/index.html
open tools/span_trace_viewer/index.html

# Linux
xdg-open tools/flow_trace_viewer/index.html
xdg-open tools/span_trace_viewer/index.html

# Windows
start tools/flow_trace_viewer/index.html
start tools/span_trace_viewer/index.html
```
