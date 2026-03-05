# Built-in MMWorkflow: memory

This built-in workflow provides a default memory access flow (read -> merge -> write).

## Files
- `workflow.json`: default workflow DSL
- `nodes/`: default node scripts that can be customized

## Customization
1. Copy this directory into your agent package (or enable via `builtin_workflows`).
2. Replace scripts under `nodes/` with business logic.
3. Extend `workflow.json` by adding nodes/edges while preserving IDs used by your agent integration.
