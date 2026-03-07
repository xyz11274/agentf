# AgentFlow Trace Viewer

A zero-dependency, single-file Web UI for visualizing AgentFlow full-chain trace data.

## Quick Start

```bash
# Open directly in browser — no server needed
open publish/tools/trace/index.html          # macOS
xdg-open publish/tools/trace/index.html     # Linux
start publish/tools/trace/index.html        # Windows
```

## Loading Trace Data

1. Run your AgentFlow application with at least one active `TraceTask`.
2. Trace data is written to `trace_output/task_<task_id>.ndjson` (one span per line).
3. Drag the `.ndjson` file into the left panel of the viewer.

Multiple files can be loaded simultaneously — sessions from all files are merged.

## Views

| Tab | Description |
|-----|-------------|
| **时序甘特图** | Horizontal bars per thread, x-axis = wall-clock time |
| **火焰图** | Hierarchical call tree by `parent_span_id`; width ∝ duration |
| **Span 列表** | Sortable table with all span fields, tags, and events |

## Task Management Panel

The left-side task management panel allows you to create, start, stop, and export
monitoring tasks *within the UI* for annotation/filtering purposes.

To create tasks that actually instrument the running process, use the C++ API:

```cpp
#include "runtime/trace/trace_manager.h"
using namespace agentflow::runtime::trace;

// Create a task filtering by session
TraceTaskFilter filter;
filter.session_id = "my-session-id";   // empty = match all

TraceManager& tm = TraceManager::instance();
tm.set_output_dir("trace_output");     // default: "trace_output"
std::string task_id = tm.create_task("my-task", filter);
tm.start_task(task_id);

// ... run your agent ...

tm.stop_task(task_id);
```

## Export

- **导出 JSON** — exports the selected session's spans as a JSON array
- **导出 CSV** — exports `trace_id,span_id,parent_span_id,kind,name,status,start_us,duration_us,thread_id`

## NDJSON Format

Each line in the output file is a JSON object:

```json
{"trace_id":"abc123","span_id":"def456","parent_span_id":"","kind":"NODE","name":"dispatch-node","status":"OK","start_us":1700000000123456,"duration_us":4521,"thread_id":140234567,"tags":{"agent_id":"agent-1"},"events":[{"timestamp_us":1700000000125000,"name":"node_success"}]}
```
