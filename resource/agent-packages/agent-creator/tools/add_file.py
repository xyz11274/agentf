"""add_file tool — add or update a file in the in-memory file tree.

Usage (LLM function calling):
  {"name": "add_file", "arguments": {"path": "agent.json", "content": "{...}"}}
  → Adds/updates the file at the given path.

Returns JSON with status and path.
"""
import json

from file_store import files

TOOL_NAME = "add_file"
TOOL_DESCRIPTION = (
    "在当前创建会话中添加或更新一个文件。"
    "path 是相对于包根目录的路径（如 agent.json, harness/skills/xxx/SKILL.md），"
    "content 是文件的完整内容。"
)
TOOL_PARAMS = {
    "type": "object",
    "properties": {
        "path": {
            "type": "string",
            "description": "文件路径，相对于包根目录（如 agent.json, harness/skills/stock-analysis/SKILL.md）"
        },
        "content": {
            "type": "string",
            "description": "文件完整内容"
        }
    },
    "required": ["path", "content"]
}


def execute(params: dict | None = None) -> str:
    if params is None:
        return json.dumps({"error": "缺少参数 path 和 content"}, ensure_ascii=False)

    path = params.get("path", "")
    content = params.get("content", "")

    if not path:
        return json.dumps({"error": "path 不能为空"}, ensure_ascii=False)

    is_new = path not in files
    files[path] = content
    return json.dumps({
        "ok": True,
        "path": path,
        "size": len(content),
        "action": "added" if is_new else "updated",
    }, ensure_ascii=False)
