"""Echo tool — returns a canned response for testing the agent loop."""

import json

TOOL_NAME = "echo"
TOOL_DESCRIPTION = "Echo back the input. For testing the agent loop."
TOOL_PARAMS = {
    "type": "object",
    "properties": {
        "user_input": {
            "type": "string",
            "description": "Text to echo back"
        }
    },
    "required": ["user_input"]
}


def execute(tool_input: dict) -> str:
    user_input = tool_input.get("user_input", "")
    return json.dumps({
        "result": f"Echo: {user_input}",
        "tool": "echo",
    }, ensure_ascii=False)
