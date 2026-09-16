---
name: movie-query
description: 观影服务（mock，固定返回示例数据）。查影讯/热映、电影推荐、影片详情与播放控制。用户提到电影、影片、想看、播放某部片时 dispatch 本 skill。
version: 1.0.0
---

# Movie Query（mock）

观影垂域 skill。固定返回成功与示例数据，用于评测链路联通；不接真实影讯 API。

## 自带工具

- `movie_query`（scripts/movie_query.py）：影讯查询 / 推荐 / 详情 / 播放

## movie_query 参数

```json
{
  "action": "recommend",   // 必填：recommend（推荐）/ now_showing（热映）/ detail（详情）/ play（播放）
  "query": "科幻",          // 可选：类型、片名或描述
  "count": 3               // 可选：返回条数（1~10，默认 3）
}
```

## 行为

- 始终返回 `{"status": "succeeded", ...}` 与固定示例影片列表
- play 动作返回固定播放成功回执
- 数据为内置样例（《流浪地球2》《奥本海默》《头号玩家》等），不要声称来自实时影讯
