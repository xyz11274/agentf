# MMNode Skill Specification

## Skill Name
MMWorkflow MMNode Execution Skill

## Capability
Execute one atomic action in workflow runtime.

## Inputs
- `node.id`: node identifier
- `node.type`: task/workflow/start/end
- `inputs`: key-value payload
- `context`: runtime context

## Invocation
1. Resolve node execution contract.
2. If `type == workflow`, resolve `workflow_ref` and handoff to nested workflow execution entry.
3. Otherwise execute by `execution_mode` (`cpp_function/shell_script/executable/go_script/python_script/java_jar/so_library`).

## Outputs
- `status`: SUCCESS/FAILED
- `outputs`: key-value payload
- `error`: optional message

## Error Handling
- retry by node config (`max_retries`, `retry_delay_ms`)
- timeout by `timeout_ms`
