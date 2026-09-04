---
name: image-gen
description: 文生图 — 把文字描述变成一张 PNG 图片（默认 wan2.7-image，可用 IMAGE_MODEL 切换）。用户明确要求画图 / 生成图片 / 配图时使用。下载图片到 workspace 并返回路径。
requires_secrets: IMAGE_API_KEY
requires_network: llm-7ztiqbmxzp9wduvz.cn-beijing.maas.aliyuncs.com
version: 1.0.0
---

# Image Gen（文生图）

调用 multimodal-generation 端点生成图片，下载到 workspace 后返回本地路径。

## 自带工具

- `generate_image`（scripts/generate_image.py）：生成一张图片

## generate_image 参数

```json
{
  "prompt": "一只在樱花树下打盹的橘猫，水彩风格",   // 必填：画面描述（中英文皆可）
  "output": "assets/orange-cat.png"                // 可选：workspace 内输出路径（默认 assets/<uuid>.png）
}
```

## 行为

- 同步调用，成功后图片下载到 `<agent包>/workspace/<output>`，返回
  `{"status": "succeeded", "files": [{"path": "...", "url": "..."}]}`
- 模型由 `IMAGE_MODEL` 配置（默认 `wan2.7-image`，当前 key 已实测开通；
  `qwen-image-3.0-pro` 等模型需另行开通）
- prompt 建议写清：主体、场景、风格、构图；不要包含任何密钥或凭据
- 失败返回 `{"status": "failed", "error": "..."}`，如实告知调用方，不要假装生成成功
- 每次调用生成一张；多张需求分多次调用（可用不同 output 路径）
