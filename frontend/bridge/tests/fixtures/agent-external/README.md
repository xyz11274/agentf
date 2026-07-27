# Agent External

L2 层外部服务验证 agent fixture，用于验证真实外部服务调用链路。

## 设计

- **coordinator** 直连真实 LLM，根据用户意图 dispatch skill
- **ext-service skill**：在沙箱内使用 `web_fetch` 工具调真实外部 API（如 `wttr.in` 等公开服务），验证 sandbox 网络连通性 + 工具执行 + 数据回传

## 与 outfit-assistant 的区别

| 维度 | agent-external | agent-outfit |
|------|---------------|-------------|
| 业务绑定 | 无（通用测试） | 穿搭助手业务 |
| 外部 API | 公开免费 API（如 wttr.in） | 业务 API（天气、图片生成等） |
| 验证目标 | 外部服务连通性 | 业务正确性 |
| API key | `AF_LLM_API_KEY` | `AF_LLM_API_KEY` + 业务 key |