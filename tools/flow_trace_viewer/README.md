# Flow Trace Viewer

A browser-based visualization tool for AgentFlow scheduling traces.

## Features

- **Sequence Diagram**: Visualize the temporal flow of events between components
- **Flow Diagram**: See the overall flow transitions between entities
- **Timing Table**: Detailed view of each event with timestamps and durations
- **Filtering**: Filter by session and dialogue ID
- **Drag & Drop**: Easy file loading

## Usage

1. Open `index.html` in a modern browser (Chrome, Firefox, Safari, Edge)
2. Either:
   - Drag and drop a trace log file onto the drop zone
   - Click "Choose File" to select a file
   - Click "Load Sample Data" to see a demo

## Log Format

The viewer expects JSON Lines (NDJSON) format, where each line is a JSON object:

```json
{"time":"2026-03-13 18:23:55.660","type":"task-exec","session":"session-001","dialogue":"12345678","from":"external","to":"dispatcher","event":"task-created","inputs":"{\"user_text\":\"帮我订一张去北京的机票\"}"}
{"time":"2026-03-13 18:23:55.661","type":"task-exec","session":"session-001","dialogue":"12345678","from":"dispatcher","to":"skill-selection","event":"workflow-scheduled"}
...
```

### Required Fields

| Field | Description |
|-------|-------------|
| `time` | Timestamp in format `YYYY-MM-DD HH:MM:SS.mmm` |
| `type` | Event category: `task-exec`, `workflow-exec`, `node-exec`, `skill-exec` |
| `session` | Session ID for grouping |
| `dialogue` | Dialogue ID for multi-dialogue support |
| `from` | Source entity name |
| `to` | Target entity name |
| `event` | Event type name |

### Optional Fields

| Field | Description |
|-------|-------------|
| `inputs` | JSON string of input data |
| `outputs` | JSON string of output data |
| `duration_us` | Duration in microseconds (for completion events) |

## Event Types

| Event | Description |
|-------|-------------|
| `task-created` | New task created by external request |
| `workflow-scheduled` | Workflow execution scheduled |
| `workflow-started` | Workflow execution started |
| `workflow-completed` | Workflow execution finished |
| `node-started` | Node execution started |
| `node-completed` | Node execution finished |
| `skill-launched` | Skill task launched |
| `skill-completed` | Skill execution finished |
| `data-exchange` | Data exchanged between components |
| `response-sent` | Response sent to external |

## Generating Traces

Set the environment variable `FLOW_TRACE_FILE` to enable file output:

```bash
export FLOW_TRACE_FILE=/path/to/trace.log
./your_agent_program
```

Or programmatically:

```cpp
#include "runtime/flow_trace.h"

// Enable file output
agentflow::runtime::FlowTraceWriter::instance().set_output_file("/path/to/trace.log");

// Or disable tracing
agentflow::runtime::FlowTraceWriter::instance().set_enabled(false);
```

## Dependencies

- **Mermaid.js** (loaded from CDN): For diagram rendering
- No build step required - just open `index.html` in a browser

## Browser Support

- Chrome 90+
- Firefox 88+
- Safari 14+
- Edge 90+

## License

Part of the AgentFlow project. See main repository for license details.