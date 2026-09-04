---
name: amap-nearby
description: 高德地图能力集 — 地址转坐标（geocode）、周边 POI 搜索（餐厅/酒店/超市/医院/加油站等）、公交站点与线路查询（bus_route_query）。用户问"附近/周边的 XX"、找地点、查公交时使用。
requires_secrets: AMAP_API_KEY
requires_network: restapi.amap.com
version: 1.0.0
---

# Amap Nearby（高德地图）

高德地图 Web 服务 API 封装。找地点、周边搜索、查公交时 dispatch 本 skill。

## 自带工具

### 1. `geocode`（scripts/geocode.py）— 地址 → 坐标

```json
{ "address": "东莞松山湖" }        // 必填：结构化或非结构化地址
```

返回 `location`（"lng,lat"）+ 省市区 + 级别。查询周边前若只有地址没有坐标，先调本工具。

### 2. `nearby_poi_search`（scripts/nearby_poi_search.py）— 周边 POI 搜索

```json
{
  "location": "113.889699,22.934925",  // location 与 address 二选一；只有 address 时自动 geocode
  "address": "深圳华为基地",
  "keywords": "川菜",                   // keywords / types 至少一个；类型代码见 Amap POI 分类表（050000=餐饮…）
  "types": "050000",
  "radius": 3000,                       // 搜索半径（米），默认 1000，最大 50000
  "offset": 10,                         // 每页条数（默认 10，最大 25）
  "page": 1                             // 页码
}
```

返回 POI 列表：名称、地址、坐标、距离（米）、电话、评分/人均（如有）。

### 3. `bus_route_query`（scripts/bus_route_query.py）— 公交站点/线路查询

```json
{
  "city": "深圳",            // 必填：城市中文名（自动转 adcode，见 data/city_code.json）或 adcode
  "keywords": "华为站"        // 必填：站点名或线路名关键字
}
```

返回匹配的站点与经停线路信息。

## 行为

- API 失败 / status != "1" 时返回 `{"status": "failed", "error": "..."}`，如实告知，不要编造 POI
- 周边搜索优先让用户明确"在哪附近"；用户没说城市时结合上下文推断或追问
- 距离单位米；评分来自 biz_ext.rating，可能为空
