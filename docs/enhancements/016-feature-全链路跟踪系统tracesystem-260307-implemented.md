## 2026-03-07 全链路跟踪系统（TraceSystem）

新增了一套零外部依赖的全链路跟踪基础设施，覆盖 Agent → Session → Workflow → Node 全执行链路。

### 新增文件

| 文件 | 说明 |
|------|------|
| `include/internal/runtime/trace/trace_types.h` | 核心数据类型：`SpanKind`、`SpanStatus`、`TraceSpan`、`TraceSession`、`TraceTask`、`TraceTaskFilter`、`SamplingContext` |
| `include/internal/runtime/trace/trace_context.h` | 线程局部 `TraceContext`（span 栈）和 RAII `ScopedSpan` |
| `include/internal/runtime/trace/trace_manager.h` | `TraceManager` 单例：任务 CRUD、session 生命周期、NDJSON 文件输出 |
| `include/internal/runtime/trace/trace_exporter.h` | 静态导出工具：`spans_to_json`、`spans_to_csv`、`span_to_ndjson` |
| `src/runtime/trace/trace_context.cpp` | TraceContext & ScopedSpan 完整实现 |
| `src/runtime/trace/trace_manager.cpp` | TraceManager 完整实现 |
| `src/runtime/trace/trace_exporter.cpp` | 导出格式实现 |
| `tools/span_trace_viewer/index.html` | 零依赖单文件 Web 可视化工具（甘特图 + 火焰图 + Span 列表 + 任务管理）|
| `tools/span_trace_viewer/README.md` | Web 工具使用说明 |
| `docs/TRACING.md` | 全链路跟踪系统完整文档 |

### 修改文件

- `CMakeLists.txt`：新增三个 trace 源文件到 `agentflow` 静态库
- `src/runtime/runtime.cpp`：
  - `#include "runtime/trace/trace_context.h"` 和 `"runtime/trace/trace_manager.h"`
  - `execute_workflow()` 开启 WORKFLOW 级 span
  - `process_node()` 使用 `ScopedSpan(SpanKind::NODE, ...)` RAII 封装节点执行，失败时调用 `set_status(ERROR)`
- `src/agent/agent.cpp`：
  - `handle_request_event()` 通过 `TraceManager::find_task()` 采样判断，匹配时调用 `begin_session()`/`end_session()`

### 设计原则

- **按需采样**：无活跃 TraceTask 时 `ScopedSpan` 为纯零开销 no-op
- **线程安全**：`thread_local TraceContext` 无锁传播，`TraceManager` 内部双 mutex（tasks / sessions 分离）
- **NDJSON 持久化**：每行一个 span，append 写，多请求并发不截断文件
- **最小侵入**：RAII ScopedSpan 不改变现有 try/catch/return 逻辑
