"""echo tool — for testing harness loop connectivity."""
import json

TOOL_NAME = "echo"
TOOL_DESCRIPTION = "回显输入内容，用于测试工具调用是否正常工作。"
TOOL_PARAMS = {
    "type": "object",
    "properties": {
        "message": {
            "type": "string",
            "description": "要回显的内容"
        }
    },
    "required": ["message"]
}


def execute(params):
    message = params.get("message", "")
    return json.dumps({"echo": message}, ensure_ascii=False)
