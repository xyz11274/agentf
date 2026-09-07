---
name: web-search
description: 网页实时搜索 — 通过 compatible-mode enable_search 内置联网插件（qwen-plus 等）检索实时信息。天气/地图/图像之外需要实时信息的场景（新闻、价格、赛事、政策等）使用。
requires_secrets: WEBSEARCH_API_KEY
requires_network: llm-7ztiqbmxzp9wduvz.cn-beijing.maas.aliyuncs.com
version: 1.1.0
---

# Web Search（网页实时搜索）

通过 compatible-mode chat completions 的 `enable_search: true` 内置联网插件检索实时信息。

> 2026-09-04 实测：本端点的 AgentStudio 仅开放 agent 管理面（无调用路由），official
> web_search MCP 未上架（AGENT_010）；可用的联网路径是 `enable_search` 插件
> （qwen-plus / qwen-max / qwen-turbo 均返回带来源的实时结果）。

## 自带工具

- `web_search`（scripts/web_search.py）：执行一次联网搜索并返回带来源的摘要

## web_search 参数

```json
{
  "query": "2026 年国庆假期高速免费政策",   // 必填：搜索问题，写清时间和主体
  "count": 5                               // 可选：期望返回的要点数（默认 5，最大 10）
}
```

## 行为

- 返回 `{"status": "succeeded", "results": "<带来源的摘要>", "sources": [...]}`
  （sources 取决于服务端是否返回 search_info）
- 模型由 `WEBSEARCH_MODEL` 配置（默认 `qwen-plus`；qwen-max 效果更好但更贵）
- 服务不可达 / 模型不可用时返回 `{"status": "failed", "error": "..."}`，
  如实告知调用方，不要编造搜索结果
- 环境变量（见 .env）：WEBSEARCH_API_KEY / WEBSEARCH_BASE_URL / WEBSEARCH_MODEL
