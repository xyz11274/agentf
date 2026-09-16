#!/usr/bin/env python3
"""device_gui tool — 设备泛化操控（mock，固定返回成功回执）。

skill-scoped tool，由 skill_executor 动态加载。
模块契约：TOOL_NAME / TOOL_DESCRIPTION / TOOL_PARAMS / execute(args)。
"""

TOOL_NAME = "device_gui"
TOOL_DESCRIPTION = (
    "Device GUI automation mock: navigate, open apps, change settings. "
    "Always succeeds with a fixed acknowledgement; performs no real device action."
)
TOOL_PARAMS = {
    "type": "object",
    "properties": {
        "action": {
            "type": "string",
            "enum": ["navigate", "open_app", "set", "generic"],
            "description": "操控类型：navigate 导航 / open_app 打开应用 / set 设置 / generic 其他",
        },
        "target": {"type": "string", "description": "目标应用或设备功能（如 '高德地图'）"},
        "params": {"type": "string", "description": "操作参数（如 '从杭州苍南广场到西湖银泰'）"},
    },
    "required": ["action"],
    "additionalProperties": False,
}


def execute(args, sandbox_endpoint=None):
    action = (args or {}).get("action")
    if not action:
        return {"status": "failed", "error": "action is required"}
    target = (args or {}).get("target") or "目标应用"
    params = (args or {}).get("params") or ""
    messages = {
        "navigate": f"已在{target}中规划路线{('：' + params) if params else ''}，最快路线约 38 分钟",
        "open_app": f"已打开{target}",
        "set": f"已完成设置{('：' + params) if params else ''}",
        "generic": f"已执行操作{('：' + params) if params else ''}",
    }
    return {
        "status": "succeeded",
        "action": action,
        "target": target,
        "message": messages.get(action, messages["generic"]),
        "mock": True,
    }
