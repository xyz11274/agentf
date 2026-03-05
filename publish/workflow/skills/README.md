# Built-in MMWorkflow: skills

This built-in workflow provides a default skills invocation flow (match -> execute -> postprocess).

## Files
- `workflow.json`: default workflow DSL
- `nodes/`: default node scripts that can be customized

## Customization
1. Keep the base workflow as a starting point.
2. Replace scripts in `nodes/` with your skill router and executors.
3. Add more skill-specific nodes/edges in `workflow.json` as needed.
