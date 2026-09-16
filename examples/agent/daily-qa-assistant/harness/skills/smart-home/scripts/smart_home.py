#!/usr/bin/env python3
"""smart_home tool — 智能家居控制（mock，固定返回成功回执）。

skill-scoped tool，由 skill_executor 动态加载。
模块契约：TOOL_NAME / TOOL_DESCRIPTION / TOOL_PARAMS / execute(args)。
"""

TOOL_NAME = "smart_home"
TOOL_DESCRIPTION = (
    "Smart home control mock: turn devices on/off, set values, apply scenes. "
    "Always succeeds with a fixed acknowledgement; controls no real device."
)
TOOL_PARAMS = {
    "type": "object",
    "properties": {
        "device": {"type": "string", "description": "设备名（灯/空调/窗帘/加湿器…）"},
        "action": {
            "type": "string",
            "enum": ["on", "off", "set", "scene"],
            "description": "操作：on 开 / off 关 / set 调参 / scene 场景",
        },
        "value": {"type": "string", "description": "设定值（如 '26度'、'50%'）"},
        "room": {"type": "string", "description": "房间（如 '客厅'、'卧室'）"},
    },
    "required": ["device", "action"],
    "additionalProperties": False,
}


def execute(args, sandbox_endpoint=None):
    device = (args or {}).get("device")
    action = (args or {}).get("action")
    if not device or not action:
        return {"status": "failed", "error": "device and action are required"}
    room = (args or {}).get("room") or "客厅"
    value = (args or {}).get("value")
    verbs = {"on": "已打开", "off": "已关闭", "set": "已调节", "scene": "已应用场景"}
    message = f"{verbs.get(action, '已操作')}{room}的{device}"
    if value:
        message += f"（{value}）"
    return {
        "status": "succeeded",
        "device": device,
        "action": action,
        "room": room,
        "value": value,
        "message": message,
        "mock": True,
    }
