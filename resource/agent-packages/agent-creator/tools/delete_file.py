"""delete_file tool — remove a file from the in-memory file tree.

Usage (LLM function calling):
  {"name": "delete_file", "arguments": {"path": "harness/skills/xxx/SKILL.md"}}
  → Removes the file at the given path.

Returns JSON with status.
"""
import json

from file_store import files

TOOL_NAME = "delete_file"
TOOL_DESCRIPTION = (
    "从当前创建会话中删除一个已添加的文件。"
    "path 是相对于包根目录的路径。"
)
TOOL_PARAMS = {
    "type": "object",
    "properties": {
        "path": {
            "type": "string",
            "description": "要删除的文件路径，相对于包根目录"
        }
    },
    "required": ["path"]
}


def execute(params: dict | None = None) -> str:
    if params is None:
        return json.dumps({"error": "缺少参数 path"}, ensure_ascii=False)

    path = params.get("path", "")
    if not path:
        return json.dumps({"error": "path 不能为空"}, ensure_ascii=False)

    if path in files:
        del files[path]
        return json.dumps({"ok": True, "path": path, "action": "deleted"}, ensure_ascii=False)
    else:
        return json.dumps({"ok": True, "path": path, "action": "not_found"}, ensure_ascii=False)
