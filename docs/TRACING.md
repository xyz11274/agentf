# AgentFlow 全链路跟踪系统 (TraceSystem)

## 概述

AgentFlow 全链路跟踪系统提供从请求进入 Agent 到执行完整个 Node 链路的全程追踪能力，支持：

- **多线程并行执行记录**：自动采集每个线程的 Span（Node/Workflow/Agent）
- **全链路时序追踪**：完整的 parent-child span 树，每个 span 包含时延、线程 ID、标签、事件
- **按需采样**：仅当存在匹配的 TraceTask 时才记录数据，默认开销为零
- **NDJSON 持久化**：每行一个 span，方便流式处理和外部工具接入
- **可视化工具**：内置 Web UI（`publish/tools/trace/index.html`）

---

## 架构

```
┌─────────────────────────────────────────────────────────┐
│  请求入口 handle_request_event()                          │
│    └─ TraceManager::find_task()  ← 采样判断              │
│    └─ TraceManager::begin_session()                      │
│         └─ TraceContext::inject(trace_id)  ← 线程注入    │
│                                                          │
│  process_node() in worker thread                         │
│    └─ ScopedSpan(SpanKind::NODE, name, tags)             │
│         └─ TraceContext::push / pop  (thread-local 栈)   │
│         └─ TraceManager::record_span()  on scope exit    │
│                                                          │
│  handle_request_event() 返回                             │
│    └─ TraceManager::end_session(trace_id)                │
│         └─ flush to trace_output/task_<id>.ndjson        │
└─────────────────────────────────────────────────────────┘
```

### 核心文件

| 文件 | 说明 |
|------|------|
| `include/internal/runtime/trace/trace_types.h` | 所有数据类型：`SpanKind`、`SpanStatus`、`TraceSpan`、`TraceTask`、`TraceTaskFilter` |
| `include/internal/runtime/trace/trace_context.h` | `TraceContext`（线程局部 span 栈）、`ScopedSpan`（RAII） |
| `include/internal/runtime/trace/trace_manager.h` | `TraceManager` 单例：任务管理 + session 生命周期 |
| `include/internal/runtime/trace/trace_exporter.h` | JSON/CSV/NDJSON 导出工具 |
| `src/runtime/trace/trace_context.cpp` | TraceContext & ScopedSpan 实现 |
| `src/runtime/trace/trace_manager.cpp` | TraceManager 完整实现 |
| `src/runtime/trace/trace_exporter.cpp` | 导出格式实现 |
| `publish/tools/trace/index.html` | 单文件 Web 可视化工具 |

---

## 快速使用

### 1. 创建监控任务

```cpp
#include "runtime/trace/trace_manager.h"

using namespace agentflow::runtime::trace;

// 配置输出目录（默认 "trace_output"）
TraceManager::instance().set_output_dir("trace_output");

// 创建过滤条件（空字符串 = 匹配所有）
TraceTaskFilter filter;
filter.session_id   = "user-123";   // 只跟踪该 session
filter.max_sessions = 10;           // 最多记录 10 次请求，0=无限制

// 创建并启动任务
std::string task_id = TraceManager::instance().create_task("prod-trace", filter);
TraceManager::instance().start_task(task_id);
```

### 2. 运行 Agent（自动采样）

满足 TraceTask 过滤条件的请求会自动被记录，无需修改业务代码。

### 3. 查看结果

```bash
# 输出文件位于
ls trace_output/task_<task_id>.ndjson

# 在浏览器中打开可视化工具
open publish/tools/trace/index.html
# 拖入 .ndjson 文件即可可视化
```

### 4. 停止任务 / 导出数据

```cpp
TraceManager::instance().stop_task(task_id);

// 导出所有 session 为 JSON 数组
auto sessions = TraceManager::instance().list_sessions(task_id);
TraceExporter::sessions_to_json_file(sessions, "my_trace.json");

// 导出 CSV
TraceExporter::spans_to_csv_file(all_spans, "my_trace.csv");
```

---

## Span 字段说明

| 字段 | 类型 | 说明 |
|------|------|------|
| `trace_id` | string | 一次请求的全局唯一 ID |
| `span_id` | string | 本 Span 的 ID |
| `parent_span_id` | string | 父 Span ID（根 span 为空） |
| `kind` | string | `AGENT` / `WORKFLOW` / `NODE` |
| `name` | string | workflow_id 或 node_name |
| `status` | string | `UNSET` / `OK` / `ERROR` |
| `start_us` | int64 | 开始时间（微秒，Unix 时间戳） |
| `duration_us` | int64 | 持续时长（微秒） |
| `thread_id` | uint64 | 执行线程哈希 ID |
| `tags` | object | 附加键值对（agent_id、workflow_id 等） |
| `events` | array | span 内打点事件（node_success、node_failed 等） |

---

## 自定义 Span 打点

在 Node 实现中可以手动创建额外 Span：

```cpp
#include "runtime/trace/trace_context.h"

using namespace agentflow::runtime::trace;

// 在 execute() 方法内
ScopedSpan sp(SpanKind::NODE, "my-custom-step",
              {{"key1", "value1"}, {"key2", "value2"}});

// ... 执行逻辑 ...

sp.add_event("cache_hit", {{"cache_key", key}});
sp.set_status(SpanStatus::OK);
// sp 析构时自动记录 end_us 并写入 TraceManager
```

---

## Web UI 使用说明

`publish/tools/trace/index.html` 为零依赖单文件 Web 工具，直接在浏览器打开：

| 功能 | 说明 |
|------|------|
| 拖入 NDJSON 文件 | 支持多文件同时加载，自动合并 |
| 时序甘特图 | 按线程分组，X 轴为时间，颜色区分 Kind |
| 火焰图 | 按 parent_span_id 构建调用树，宽度正比于耗时 |
| Span 列表 | 完整字段表格，支持 tags/events 展示 |
| 任务管理 | 页面内创建/启停任务（需配合 C++ API 实际采样） |
| 导出 JSON/CSV | 导出当前 Session 数据 |

---

## 性能影响

- **零活跃任务**：`TraceManager::find_task()` 仅检查 tasks map（读锁），`ScopedSpan` 构造器检查 `TraceContext::trace_id()` 为空后立即返回，无任何分配
- **有活跃任务**：每个 Span 一次 mutex 写（`record_span`）+ 一次 vector push，开销约 1-2μs
- **线程安全**：`thread_local TraceContext` 无锁；`tasks_mutex_` / `sessions_mutex_` 独立，不互相阻塞

---

## Metrics 集成

全链路跟踪系统与 `MetricsCollector`（`include/internal/runtime/metrics.h`）深度集成，实现了**单请求全链路跟踪**与**全局聚合统计**的双轨并行。

### 工作机制

```
ScopedSpan 析构
  └─ TraceManager::record_span(span)
       └─ feed_metrics(span)            ← 增量更新全局 MetricsCollector
            ├─ agentflow.nodes.executed ++
            ├─ agentflow.nodes.errors ++ (if ERROR)
            └─ agentflow.node.duration_ms.record(ms)

TraceManager::end_session(trace_id)
  ├─ 更新 agentflow.requests.total ++
  ├─ 更新 agentflow.requests.errors ++ (if any ERROR span)
  ├─ agentflow.request.duration_ms.record(end-start ms)
  └─ flush_session_internal()
       ├─ 写 task_<id>.ndjson (span 追加)
       └─ 写 metrics.json     (全局统计快照，覆盖写)
```

### 全局 Metrics Key 一览

| Key | 类型 | 说明 |
|-----|------|------|
| `agentflow.requests.total` | Counter | 已完成的被跟踪请求总数 |
| `agentflow.requests.errors` | Counter | 含 ERROR span 的请求数 |
| `agentflow.request.duration_ms` | Histogram | 请求端到端耗时（ms） |
| `agentflow.nodes.executed` | Counter | 节点执行次数 |
| `agentflow.nodes.errors` | Counter | 节点执行失败次数 |
| `agentflow.node.duration_ms` | Histogram | 节点执行耗时（ms） |
| `agentflow.workflows.executed` | Counter | Workflow 执行次数 |
| `agentflow.workflow.duration_ms` | Histogram | Workflow 执行耗时（ms） |
| `agentflow.active_sessions` | Gauge | 当前正在跟踪中的并发请求数 |

### metrics.json 格式

每次请求结束后，`trace_output/metrics.json` 自动覆盖更新：

```json
{
  "metrics": [
    {"name":"agentflow.requests.total","type":"counter","value":42},
    {"name":"agentflow.node.duration_ms","type":"histogram","count":120,"sum":3540.5,"avg":29.5,"p50":10.0,"p90":100.0,"p95":250.0,"p99":500.0},
    ...
  ]
}
```

### Web UI 统计指标页

1. 打开 `publish/tools/trace/index.html`
2. 点击侧边栏"📊 加载 metrics.json"按钮，选择 `trace_output/metrics.json`
3. 页面切换到**统计指标**视图：
   - **计数器 & 瞬时值**：卡片展示每个 Counter/Gauge 的当前值
   - **直方图**：每个 Histogram 显示 p50/p90/p95/p99 标注 + 桶分布条形图
   - 支持**导出 JSON**按钮将当前 metrics 快照下载

### 代码示例：读取全局 Metrics

```cpp
#include "runtime/metrics.h"
using namespace agentflow::runtime;

auto mc = MetricsCollectorFactory::get_default();

// 读取请求总数
auto total = std::dynamic_pointer_cast<CounterMetric>(
    mc->get_metric("agentflow.requests.total"));
if (total) {
    std::cout << "Total requests: " << total->get_value() << std::endl;
}

// 导出完整 JSON
std::string json = mc->export_json();

// 导出 Prometheus 格式
std::string prom = mc->export_prometheus();
```
