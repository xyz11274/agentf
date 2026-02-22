# Workflow / Node Development Guide

## Built-in workflows
Built-in templates are provided under `publish/workflow/`:
- `memory/`
- `skills/`
- `tools/`

## Workflow DSL specification sample
Canonical sample is now located at:
- `publish/workflow/workflow_sample.jsonc`

## Workflow development
1. Define workflow id/name/version.
2. Define nodes with `execution_mode` and executable binding (`command` / symbol).
3. Define edges for control/data flow.

## Node development
- Node specification: `publish/workflow/NODE_DEVELOPMENT_SPEC.md`
- Node API reference: `publish/workflow/NODE_API_REFERENCE.md`

## Packaging integration
When `builtin_workflows` is configured in agent `package.json`, `publish/agent_build.sh` stages selected built-ins into packaged artifact content.
