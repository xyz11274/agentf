# openJiuwen vendored compatibility layer

This directory stores the in-repository Python compatibility/runtime surface used by AgentFlow examples.

## Scope

- `python/openjiuwen/`: lightweight `openjiuwen.core.*` compatibility modules aligned with the public examples used by `agent-core`.
- `python/jiuwenclaw/`: vendored `jiuwenclaw` runtime shim used by `examples/agent/claw_agent/`.

The intent is to keep AgentFlow examples runnable and testable from this repository without requiring an external clone at runtime.
