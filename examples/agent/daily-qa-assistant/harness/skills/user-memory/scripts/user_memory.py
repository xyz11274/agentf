#!/usr/bin/env python3
"""user_memory tool — 用户偏好记忆（mock，固定返回示例偏好）。

skill-scoped tool，由 skill_executor 动态加载。
模块契约：TOOL_NAME / TOOL_DESCRIPTION / TOOL_PARAMS / execute(args)。
"""

TOOL_NAME = "user_memory"
TOOL_DESCRIPTION = (
    "User preference memory mock: read returns fixed sample preferences, write "
    "acknowledges without persisting. Always succeeds."
)
TOOL_PARAMS = {
    "type": "object",
    "properties": {
        "action": {
            "type": "string",
            "enum": ["read", "write"],
            "description": "read 读取偏好 / write 记录偏好",
        },
        "key": {"type": "string", "description": "偏好项名称（如 '电影偏好'）"},
        "value": {"type": "string", "description": "write 时的偏好值"},
    },
    "required": ["action"],
    "additionalProperties": False,
}

_PREFERENCES = {
    "电影偏好": "科幻",
    "口味": "不吃辣",
    "出行": "高铁优先",
}


def execute(args, sandbox_endpoint=None):
    action = (args or {}).get("action")
    if not action:
        return {"status": "failed", "error": "action is required"}
    if action == "write":
        key = (args or {}).get("key") or "偏好"
        return {
            "status": "succeeded",
            "action": "write",
            "message": f"已记住你的{key}：{(args or {}).get('value', '')}",
            "mock": True,
        }
    return {
        "status": "succeeded",
        "action": "read",
        "preferences": dict(_PREFERENCES),
        "message": "已读取用户偏好（示例数据）",
        "mock": True,
    }
