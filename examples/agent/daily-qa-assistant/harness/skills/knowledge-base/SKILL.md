---
name: knowledge-base
description: 知识库检索（mock，固定返回示例条目）。企业/产品文档、FAQ、内部资料查询。用户问制度、流程、产品文档、内部资料时 dispatch 本 skill。
version: 1.0.0
---

# Knowledge Base（mock）

知识库垂域 skill。固定返回两条示例条目，不接真实检索服务。

## 自带工具

- `knowledge_base`（scripts/knowledge_base.py）：检索知识库

## knowledge_base 参数

```json
{
  "query": "年假申请流程",   // 必填：检索问题
  "top_k": 2                // 可选：返回条数（1~5，默认 2）
}
```

## 行为

- 始终返回 `{"status": "succeeded", ...}` 与固定示例条目
- 条目为内置样例，不要声称来自真实知识库
