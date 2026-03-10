# ClawAgent

ClawAgent is a dispatcher-first AgentFlow example that routes user messages into a vendored `jiuwenclaw` runtime and renders responses in a terminal chat UI.

## Layout

```text
examples/agent/claw_agent/
  agent.json
  package.json
  dispatcher_workflow.json
  coordinator_workflow.json
  claw_skill_workflow.json
  nodes/python/
  tests/
```

## Run

```bash
./build.sh build
./build/example_claw_agent_tui examples/agent/claw_agent
```

## Smoke test

```bash
bash examples/agent/claw_agent/tests/test_claw_agent_tui.sh
```
