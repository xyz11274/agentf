# Span Trace Viewer（全链路跟踪可视化工具）

零依赖单文件 Web 工具，用于可视化 AgentFlow 全链路 Span 跟踪数据。

## 快速启动

```bash
# 直接在浏览器中打开，无需服务器
open tools/span_trace_viewer/index.html          # macOS
xdg-open tools/span_trace_viewer/index.html     # Linux
start tools/span_trace_viewer/index.html        # Windows
```

## 加载跟踪数据

1. 在代码中创建并启动 `TraceTask`（见下方 C++ API）。
2. 跟踪数据写入 `trace_output/task_<task_id>.ndjson`，每行一个 Span。
3. 将 `.ndjson` 文件拖入左侧面板，或点击上传按钮加载。

支持同时加载多个文件，所有 Session 会自动合并显示。

## 视图说明

| 标签页 | 说明 |
|--------|------|
| **时序甘特图** | 按线程分组展示 Span 横条，X 轴为绝对挂钟时间 |
| **火焰图** | 按 `parent_span_id` 构建调用树，宽度正比于耗时 |
| **Span 列表** | 含所有字段的可排序表格，支持 tags 和 events 展示 |

## 左侧任务管理面板

任务管理面板支持在 UI 内创建、启停、导出监控任务（用于标注和过滤）。

如需对运行中的进程实际采样，请使用 C++ API：

```cpp
#include "runtime/trace/trace_manager.h"
using namespace agentflow::runtime::trace;

// 设置输出目录（默认 "trace_output"）
TraceManager::instance().set_output_dir("trace_output");

// 创建过滤条件（空字符串 = 匹配所有）
TraceTaskFilter filter;
filter.session_id = "my-session-id";   // 空 = 匹配全部

std::string task_id = TraceManager::instance().create_task("my-task", filter);
TraceManager::instance().start_task(task_id);

// ... 运行 Agent ...

TraceManager::instance().stop_task(task_id);
```

## 导出功能

- **导出 JSON** — 将选定 Session 的所有 Span 导出为 JSON 数组
- **导出 CSV** — 导出 `trace_id,span_id,parent_span_id,kind,name,status,start_us,duration_us,thread_id`

## Metrics 统计视图

除 Span 跟踪外，本工具还支持加载 `trace_output/metrics.json` 查看全局统计指标：

1. 点击侧边栏"📊 加载 metrics.json"按钮
2. 切换至**统计指标**视图：
   - 计数器 & 瞬时值：卡片展示 Counter/Gauge
   - 直方图：p50/p90/p95/p99 分位数 + 桶分布条形图
   - 支持导出当前 metrics 快照

## NDJSON 格式说明

输出文件每行一个 JSON 对象（TraceSpan）：

```json
{"trace_id":"abc123","span_id":"def456","parent_span_id":"","kind":"NODE","name":"dispatch-node","status":"OK","start_us":1700000000123456,"duration_us":4521,"thread_id":140234567,"tags":{"agent_id":"agent-1"},"events":[{"timestamp_us":1700000000125000,"name":"node_success"}]}
```

### Span 字段说明

| 字段 | 类型 | 说明 |
|------|------|------|
| `trace_id` | string | 一次请求的全局唯一 ID |
| `span_id` | string | 本 Span 的 ID |
| `parent_span_id` | string | 父 Span ID（根 Span 为空） |
| `kind` | string | `REQUEST` / `AGENT` / `WORKFLOW` / `NODE` / `EDGE` |
| `name` | string | Span 名称（workflow_id 或 node_name） |
| `status` | string | `RUNNING` / `OK` / `ERROR` / `CANCELLED` |
| `start_us` | int64 | 开始时间（微秒 Unix 时间戳） |
| `duration_us` | int64 | 持续时长（微秒） |
| `thread_id` | uint64 | 执行线程 ID |
| `tags` | object | 附加键值对（agent_id、workflow_id 等） |
| `events` | array | Span 内打点事件（timestamp_us + name） |

## 数据来源

此工具对应 `TraceManager` 全链路跟踪系统，详见 `docs/TRACING.md`。

如需可视化调度流消息流转（FlowTrace），请使用 `tools/flow_trace_viewer/`。
