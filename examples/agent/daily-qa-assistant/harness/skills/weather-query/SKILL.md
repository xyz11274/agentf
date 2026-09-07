---
name: weather-query
description: 心知天气实时天气与预报查询。输入城市名（中文/拼音/adcode），返回实时天气现象、温度、湿度、风向，以及未来 3 天预报。零第三方依赖。
requires_secrets: SENIVERSE_API_KEY
requires_network: api.seniverse.com
version: 1.0.0
---

# Weather Query（心知天气）

查询真实天气数据（心知天气 v3 API）。用户问天气、气温、下雨、穿衣建议时 dispatch 本 skill。

## 自带工具

- `weather_query`（scripts/weather_query.py）：查实时天气 + 未来预报

## weather_query 参数

```json
{
  "location": "东莞",          // 必填：城市中文名 / 拼音 / adcode（如 dongguan / 441900）
  "days": 0,                   // 可选：0=仅实况（默认），1~3=附带未来 N 天预报
  "unit": "c"                  // 可选：c 摄氏度（默认）/ f 华氏度
}
```

## 行为

- 返回结构化天气数据：天气现象、温度、湿度、风向风力、更新时间
- `days > 0` 时附带逐日预报（白天/夜间现象、最高/最低温、降水概率如有）
- API 失败时返回 `{"status": "failed", "error": "..."}`，如实告知调用方，不要编造
- 只接受城市级查询；行政区划（区/县）直接传名称即可（API 自动匹配）
