# Agent Connectivity

L0 层连通性验证 agent fixture，用于验证 daemon 启动 + 真实 LLM 连通性。

## 设计

- **单 coordinator 节点**:直接调用 `agentflow_runtime.coordinator_decide`,走真实 LLM
- **passthrough pre_skill**:dispatcher → `next_skills=["pre_skill"]` → framework 自动 forward 给 coordinator(参照 outfit-assistant 的 `dummy_pre_skill` 机制)
- **session_pool_size=1**:单会话足够验证连通性

> ⚠️ 设计要点:dispatcher 必须配 `next_skills` 透传到 skill,framework 才会自动 forward
> 给 coordinator task。若 `next_skills=[]`,dispatcher 会把消息当成"无 workflow"消费掉,
> coordinator 永不被触发,LLM 调用不会发生。

## 用途

被 `frontend/bridge/tests/test_e2e_minimal_agent.py` 使用,验证:
1. daemon 启动 + agent 注册
2. 单轮真实 LLM 调用返回非空内容
3. 多轮对话上下文保持

## 与 outfit-assistant 的关系

| 场景 | fixture | LLM 模式 |
|------|---------|---------|
| 真实 LLM 连通性(精简) | **agent-connectivity**(本 fixture) | 真实 LLM |
| 真实 LLM Bridge REST SSE | `test_e2e_rest_chat_session.py` 共享 fixture | 真实 LLM |
| outfit 专属路径(skill 调度等) | `outfit-assistant` | mock LLM |

agent-connectivity 仅验证"daemon → coordinator → 真实 LLM → 响应"链路完整性,
不验证 coordinator 调度 skill 的复杂路径(那条路径由 outfit-assistant 专属 e2e 覆盖)。
