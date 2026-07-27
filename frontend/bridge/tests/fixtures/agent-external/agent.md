---
name: agent-external
description: L2 外部服务验证 agent — 调真实外部 API，验证 sandbox 内工具链 + 外部服务连通性
version: 1.0.0
---

# Agent External

L2 层外部服务验证 agent fixture，用于验证真实外部服务调用链路。

## 行为

- **coordinator** 直连真实 LLM，根据用户意图 dispatch skill
- **ext-service skill**：在沙箱内使用 `web_fetch` 工具调真实外部 API（如公开天气 API），验证 sandbox 网络连通性 + 工具执行 + 数据回传

## Skills

| Skill | 用途 | 是否调用外部 API |
|-------|------|----------------|
| `ext-service` | 在沙箱内调真实外部 API 并返回结果 | ✅ 是 |

## 设计要点

- 不绑定任何业务逻辑（outfit、穿搭等）
- 验证的是"外部服务连通性"而非"业务正确性"
- 使用 `harness_agent` template 的基础设施
- 需要真实 LLM（`AF_LLM_API_KEY`）+ 沙箱网络出站规则