# Built-in MMWorkflow: tools

This built-in workflow provides a default MCP/tools invocation flow (select tool -> call tool -> format result).

## Files
- `workflow.json`: default workflow DSL
- `nodes/`: default node scripts that can be customized

## Customization
1. Enable this built-in workflow from agent package metadata.
2. Replace tool scripts under `nodes/` with real MCP tool calls.
3. Extend node graph for retries, fallback tools, and post-processing.
