# Agent Sandbox

L1 层沙箱验证 agent fixture，用于验证沙箱模式下的 E2E 全链路。

## 设计

- **问答场景**：coordinator 直连 LLM，返回文本响应
- **图片生成场景**：coordinator 调度 `draw-image` skill，skill 在沙箱内使用工具生成测试图片
- **图片生成 100% 本地**：用 Python 标准库（struct + zlib）手动构造 PNG，不调用任何外部 API

## 测试覆盖

| 测试 | 验证链路 |
|------|---------|
| 问答 | bridge HTTP POST → daemon → agent → coordinator → LLM → 响应 |
| 图片生成 | bridge HTTP POST → daemon → agent → coordinator → skill_executor → e2b 沙箱 → 工具执行 → 图片生成 → 产物收集 → 响应 |

## 与 agent-connectivity 的关系

| 维度 | agent-connectivity | agent-sandbox |
|------|---------------|-------------------|
| 用途 | 真实 LLM 连通性验证 | 沙箱模式全链路测试 |
| Skills | 无 | 1 个（draw-image） |
| 沙箱集成 | 无 | 完整（e2b/Docker） |
| 图片生成 | 无 | 有（纯本地，不调 API） |
| LLM 模式 | 真实 LLM 优先 | mock LLM（不依赖真实 API） |