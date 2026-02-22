# Workflow / Node Development Guide

## Built-in workflows
Built-in templates are provided under `publish/workflow/`:
- `memory/`
- `skills/`
- `tools/`
- `pattern/serial_workflow.json`
- `pattern/parallel_workflow.json`
- `pattern/cycled_workflow.json`

## Workflow DSL specification sample
Canonical sample is now located at:
- `publish/workflow/workflow_sample.jsonc`

## Workflow development
1. Define workflow id/name/version.
2. Define nodes with `execution_mode` and executable binding (`command` / symbol).
3. Define edges for control/data flow.
4. Use unified edge control model:
   - `control.type`: `if` / `loop` / `fork` / `join` / `async` / `goto`
   - `control.*`: per-mode parameters (`expression`, `max_iterations`, `join_group`, etc.)

## Node development
- Node specification: `publish/workflow/NODE_DEVELOPMENT_SPEC.md`
- Node API reference: `publish/workflow/NODE_API_REFERENCE.md`
- Node pattern templates: `publish/workflow/node_patterns/`

## Packaging integration
When `builtin_workflows` is configured in agent `package.json`, `publish/agent_build.sh` stages selected built-ins into packaged artifact content.
