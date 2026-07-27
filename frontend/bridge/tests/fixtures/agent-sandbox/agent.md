---
name: agent-sandbox
description: L1 沙箱验证 agent — 验证 E2B/Docker 沙箱模式下问答 + 图片生成全链路
version: 1.0.0
---

# Agent Sandbox

L1 层沙箱验证 agent fixture，用于验证沙箱模式下的 E2E 全链路。

## 行为

- **问答场景**：coordinator 直连 LLM，返回文本响应
- **图片生成场景**：coordinator 调度 `draw-image` skill，skill 在沙箱内使用工具生成测试图片

## Skills

| Skill | 用途 | 是否调用外部 API |
|-------|------|----------------|
| `draw-image` | 在沙箱内生成测试图片（纯本地，不调外部 API） | ❌ 否 |

## 设计要点

- 不依赖真实外部 API（图片生成用 Python 标准库在沙箱内绘制）
- 使用 `harness_agent` template 的基础设施
- 支持 mock LLM 和真实 LLM 两种模式