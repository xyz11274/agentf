---
name: weather_query
description: 查询指定城市的实时天气和穿衣指数
trigger: 天气查询, 穿衣建议, 今日天气, 城市天气
---

# Weather Query Skill

查询指定城市的实时天气、穿衣指数和紫外线指数。

## 用途

当 coordinator 需要天气数据来生成穿搭建议时，委派此 skill。

## 行为

- 接收城市名称（中文或拼音）
- 通过 `scripts/weather_query.py` 工具查询实时天气（温度、体感、湿度、天气描述、风向）
- 查询生活指数（穿衣指数、紫外线指数）
- 返回结构化天气数据供 coordinator 综合判断

## 自带工具

- `weather_query`（位于 `scripts/weather_query.py`）：调用和风天气 API，返回结构化天气数据

## 前提条件

环境变量：`QWEATHER_API_KEY`、`QWEATHER_API_HOST`
