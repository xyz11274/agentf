---
name: draw-image
description: 在沙箱内生成测试图片（纯本地、不调外部 API），用于 E2E 沙箱测试
---

# Draw Image

在沙箱内生成一张测试图片，用于验证 E2E 沙箱全链路。

## 行为

1. 调用 `scripts/generate_test_image.py` 生成测试图片
2. 输出图片文件到工作目录
3. 返回图片路径供产物收集

## 依赖

- Python 标准库（无需额外安装）
- 沙箱内 bash 工具

## 不变量

- 生成的图片文件大小 > 1000 bytes
- 图片格式为 PNG
- 不调用外部 API