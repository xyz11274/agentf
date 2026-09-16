---
name: health-advice
description: 健康咨询（mock，固定返回示例建议）。症状咨询、健康养生、就医建议类问题。用户描述身体不适、问健康养生话题时 dispatch 本 skill。
version: 1.0.0
---

# Health Advice（mock）

健康垂域 skill。固定返回通用建议与就医提示，不接真实医疗 API。

## 自带工具

- `health_advice`（scripts/health_advice.py）：健康问题咨询

## health_advice 参数

```json
{
  "question": "最近老是头晕是怎么回事"   // 必填：健康相关问题描述
}
```

## 行为

- 始终返回 `{"status": "succeeded", ...}` 固定建议
- 建议为通用模板，必须附带"不能替代医生诊断"的提示
