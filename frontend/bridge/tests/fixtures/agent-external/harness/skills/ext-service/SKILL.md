---
name: ext-service
description: 在沙箱内调真实外部 API（如公开天气 API），验证 sandbox 网络连通性 + 工具执行 + 数据回传
---

# External Service Skill

调用真实外部 API，验证沙箱网络出站 + 工具执行全链路。

## 行为

1. 使用 `web_fetch` 工具调真实外部 API（如 `wttr.in` 等公开服务）
2. 返回 API 响应结果
3. 不绑定任何业务逻辑

## 依赖

- 沙箱内 `web_fetch` 工具
- 沙箱网络出站规则允许访问外部 API

## 不变量

- 不依赖任何业务 API key（使用公开免费 API）
- 响应的数据格式与外部 API 一致