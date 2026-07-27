---
name: agent-connectivity
description: L0 连通性验证 agent — 单 coordinator 节点直连 LLM, 用于真实 LLM 连通性 E2E
version: 1.0.0
---

# Agent Connectivity

L0 层连通性验证 agent fixture，用于验证 daemon 启动 + 真实 LLM 连通性。

## 行为

- 单 coordinator 节点直连 LLM
- dispatcher → dummy_pre_skill(passthrough) → framework 自动 forward 给 coordinator
- 不调度任何业务 skill
