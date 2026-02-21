# AgentFlow Workflow DSL Reference

## Overview

The AgentFlow workflow DSL is a JSON (or JSONC — JSON with comments) document that fully
describes a directed acyclic graph (DAG) of computational nodes connected by typed edges.

The canonical reference sample is [`workflow_sample.jsonc`](workflow_sample.jsonc).  Every
field in that file maps 1-to-1 with a `JsonHelper` call inside
`src/workflow/dsl/workflow_parser.cpp`.

---

## Top-level structure

```jsonc
{
  "version": "1.0",            // required – DSL schema version
  "id":      "my_workflow",    // required – unique workflow identifier
  "name":    "My Workflow",    // optional – display name
  "description": "…",         // optional
  "dsl_file":    "path/to.jsonc",  // optional – stored for traceability
  "metadata": { … },          // optional – freeform, not parsed
  "nodes": [ … ],             // required – array of node objects
  "edges": [ … ]              // required – array of edge objects
}
```

---

## Node fields

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `id` | string | **required** | Unique node identifier within the workflow |
| `name` | string | `id` | Human-readable display name |
| `type` | string | `"default"` | Node category (e.g. `"source"`, `"transform"`, `"sink"`) |
| `priority` | int | `0` | Scheduling priority; higher = earlier |
| `execution_mode` | enum string | `"cpp_function"` | How the node executes (see below) |
| `command` | string | `""` | Script/binary path for external execution modes |
| `working_dir` | string | `""` | Working directory for external execution |
| `args` | string array | `[]` | Extra arguments appended to `command` |
| `function_name` | string | `""` | Exported symbol name for `so_library` mode |
| `main_class` | string | `""` | Java main class for future JAR mode |
| `inputs` | string array | `[]` | Named input data ports |
| `outputs` | string array | `[]` | Named output data ports |
| `max_retries` | int | `3` | Retry attempts on failure |
| `retry_delay_ms` | int | `1000` | Delay between retries (milliseconds) |
| `timeout_ms` | int | `30000` | Single-attempt wall-clock timeout (`0` = unlimited) |
| `memory_limit_mb` | int | `0` | Memory ceiling in MiB (`0` = unlimited) |
| `cpu_limit` | int | `0` | CPU cap in % of one core (`0` = unlimited) |
| `enable_hot_reload` | bool | `true` | Participate in live SO-library hot-reload |

### `execution_mode` values

| Value | Behaviour |
|-------|-----------|
| `"cpp_function"` | Calls a registered C++ lambda (`Config::cpp_function`) |
| `"shell_script"` | Runs `command` via `/bin/sh` inside `working_dir` |
| `"executable"` | Runs `command` as a direct subprocess |
| `"python_script"` | Runs `command` with the system Python interpreter |
| `"so_library"` | `dlopen(command)` and invokes the symbol `function_name` |

---

## Edge fields

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `id` | string | **required** | Unique edge identifier |
| `source` | string | **required** | Upstream node `id` |
| `target` | string | **required** | Downstream node `id` |
| `source_outputs` | string array | `[]` | Output ports of `source` to forward |
| `target_inputs` | string array | `[]` | Input ports of `target` to receive data |
| `transfer_mode` | enum string | `"shared_memory"` | Data transport mechanism (see below) |
| `data_formats` | string array | `[]` | MIME types or custom format tags |
| `supported_events` | string array | `[]` | Lifecycle events this edge reacts to |
| `condition_expression` | string | `""` | CEL expression; empty = always active |
| `priority` | int | `0` | Tie-break priority among outgoing edges |

### `transfer_mode` values

| Value | Behaviour |
|-------|-----------|
| `"shared_memory"` | Zero-copy in-process pointer passing (default) |
| `"message_queue"` | Serialised through an async bounded queue |
| `"remote_transfer"` | Serialised and sent over a network connection |

### `supported_events` values

`"start"`, `"end"`, `"fail"`, `"data_ready"`, `"timeout"`

---

## Parser auto-generation guide

Each leaf field follows one of four patterns recognised by `WorkflowParser`:

```cpp
// String
config.field = JsonHelper::get_string(obj, "field", "default");

// Integer (cast to int as needed)
config.field = static_cast<int>(JsonHelper::get_int(obj, "field", 0));

// Boolean
config.field = JsonHelper::get_bool(obj, "field", true);

// String array
auto arr = JsonHelper::get_array(obj, "field");
if (arr) {
    for (size_t i = 0; i < arr->array_size(); ++i) {
        auto v = arr->array_at(i);
        if (v && v->is_string()) config.field.push_back(v->as_string());
    }
}
```

A code generator consuming `workflow_sample.jsonc` can:

1. Walk every JSON key in the `nodes[0]` and `edges[0]` objects.
2. Infer the helper call from the value type: `string` → `get_string`, `number` → `get_int`
   or `get_double`, `bool` → `get_bool`, `array` → `get_array` + element loop.
3. Emit a C++ statement for each key, inserting the field name and the sample value as the
   default argument.
4. For enum-like string fields (e.g. `execution_mode`, `transfer_mode`), emit an
   if/else-if chain mapping each documented string to the corresponding C++ enum value.

---

## Validation rules

- `workflow.id` must be non-empty.
- `node.id` must be non-empty and unique within the workflow.
- `edge.id` must be non-empty and unique within the workflow.
- `edge.source` and `edge.target` must reference existing node `id` values.
- Cycles in the node graph are detected by `WorkflowValidator` (not the parser).
