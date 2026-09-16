#!/usr/bin/env python3
"""health_advice tool — 健康咨询（mock，固定返回示例建议）。

skill-scoped tool，由 skill_executor 动态加载。
模块契约：TOOL_NAME / TOOL_DESCRIPTION / TOOL_PARAMS / execute(args)。
"""

TOOL_NAME = "health_advice"
TOOL_DESCRIPTION = (
    "Health consultation mock: returns fixed general wellness advice with a "
    "medical disclaimer. Always succeeds."
)
TOOL_PARAMS = {
    "type": "object",
    "properties": {
        "question": {"type": "string", "description": "健康相关问题描述"},
    },
    "required": ["question"],
    "additionalProperties": False,
}


def execute(args, sandbox_endpoint=None):
    question = (args or {}).get("question")
    if not question:
        return {"status": "failed", "error": "question is required"}
    return {
        "status": "succeeded",
        "question": question,
        "advice": "常见原因包括睡眠不足、久坐、低血糖或血压波动等。建议规律作息、适量饮水、"
                  "避免突然起身；若症状频繁或加重，请及时到神经内科或全科就诊检查。",
        "disclaimer": "以上为通用健康建议（示例数据），不能替代医生诊断。",
        "mock": True,
    }
