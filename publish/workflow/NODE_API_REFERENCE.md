# Node API Reference

## Runtime-facing API surface
Node implementations can rely on workflow runtime conventions:

1. Invocation metadata passed through input payload (`trace_id/workflow_id/node_id`).
2. Data exchange via JSON payload fields aligned with workflow `inputs/outputs`.
3. Retry and timeout behavior controlled by workflow node configuration (`max_retries`, `retry_delay_ms`, `timeout_ms`).

## Execution mode mapping
- `shell_script`: executes command via shell
- `executable`: executes binary directly
- `python_script`: executes Python script
- `so_library`: loads shared object and calls configured symbol
- `cpp_function`: framework-registered function

## Example output
```json
{
  "status": "SUCCESS",
  "payload": {
    "result": "ok"
  }
}
```
