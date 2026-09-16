#!/usr/bin/env python3
"""movie_query tool — 观影服务（mock，固定返回示例数据）。

skill-scoped tool，由 skill_executor 动态加载。
模块契约：TOOL_NAME / TOOL_DESCRIPTION / TOOL_PARAMS / execute(args)。
"""

TOOL_NAME = "movie_query"
TOOL_DESCRIPTION = (
    "Movie domain mock: recommend movies, list now-showing, get movie detail, "
    "or play a movie. Always succeeds with fixed sample data."
)
TOOL_PARAMS = {
    "type": "object",
    "properties": {
        "action": {
            "type": "string",
            "enum": ["recommend", "now_showing", "detail", "play"],
            "description": "操作类型：recommend 推荐 / now_showing 热映 / detail 详情 / play 播放",
        },
        "query": {"type": "string", "description": "类型、片名或描述（如 '科幻'、'奥本海默'）"},
        "count": {"type": "integer", "minimum": 1, "maximum": 10, "description": "返回条数（默认 3）"},
    },
    "required": ["action"],
    "additionalProperties": False,
}

_MOVIES = [
    {"title": "流浪地球2", "type": "科幻", "rating": 7.9, "actors": ["吴京", "刘德华"], "year": 2023},
    {"title": "奥本海默", "type": "剧情", "rating": 8.8, "actors": ["基里安·墨菲"], "year": 2023},
    {"title": "头号玩家", "type": "科幻", "rating": 8.6, "actors": ["泰伊·谢里丹"], "year": 2018},
    {"title": "泰坦尼克号", "type": "爱情", "rating": 9.4, "actors": ["莱昂纳多·迪卡普里奥"], "year": 1997},
    {"title": "盗梦空间", "type": "科幻", "rating": 9.3, "actors": ["莱昂纳多·迪卡普里奥"], "year": 2010},
]


def execute(args, sandbox_endpoint=None):
    action = (args or {}).get("action")
    if not action:
        return {"status": "failed", "error": "action is required"}
    query = (args or {}).get("query", "")
    count = int((args or {}).get("count") or 3)
    movies = _MOVIES[:max(1, min(count, len(_MOVIES)))]
    if action == "play":
        return {
            "status": "succeeded",
            "action": "play",
            "message": f"已开始播放《{query or _MOVIES[0]['title']}》",
            "mock": True,
        }
    if action == "detail":
        m = _MOVIES[0]
        return {"status": "succeeded", "action": "detail", "movie": m, "mock": True}
    return {
        "status": "succeeded",
        "action": action,
        "query": query,
        "list": movies,
        "message": f"为你找到 {len(movies)} 部影片（示例数据）",
        "mock": True,
    }
