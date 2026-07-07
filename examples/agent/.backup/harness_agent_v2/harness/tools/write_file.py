"""write_file tool — write content to a file on the local filesystem."""

import json
import os


TOOL_NAME = "write_file"
TOOL_DESCRIPTION = "Write content to a file on the local filesystem. Creates parent directories if needed. Existing files will be overwritten."
TOOL_PARAMS = {
    "type": "object",
    "properties": {
        "file_path": {
            "type": "string",
            "description": "Absolute or relative path to the file to write"
        },
        "content": {
            "type": "string",
            "description": "Content to write to the file"
        },
        "append": {
            "type": "boolean",
            "description": "If true, append to existing file instead of overwriting (default false)",
            "default": False
        }
    },
    "required": ["file_path", "content"]
}


def execute(tool_input: dict) -> str:
    file_path = tool_input.get("file_path", "")
    content = tool_input.get("content", "")
    append = tool_input.get("append", False)

    if not file_path:
        return json.dumps({"error": "file_path is required", "tool": "write_file"},
                          ensure_ascii=False)
    if content is None:
        content = ""

    # Resolve and validate path
    resolved = os.path.abspath(file_path)
    cwd = os.path.abspath(os.getcwd())
    if not resolved.startswith(cwd):
        return json.dumps({
            "error": f"Access denied: path '{file_path}' is outside the working directory '{cwd}'",
            "tool": "write_file"
        }, ensure_ascii=False)

    try:
        # Create parent directories
        parent = os.path.dirname(resolved)
        if parent:
            os.makedirs(parent, exist_ok=True)

        mode = "a" if append else "w"
        with open(resolved, mode, encoding="utf-8") as f:
            f.write(content)

        return json.dumps({
            "success": True,
            "file_path": resolved,
            "char_count": len(content),
            "action": "appended" if append else "written",
            "tool": "write_file",
        }, ensure_ascii=False)

    except PermissionError:
        return json.dumps({
            "error": f"Permission denied: {file_path}",
            "tool": "write_file"
        }, ensure_ascii=False)
    except IsADirectoryError:
        return json.dumps({
            "error": f"Path is a directory: {file_path}",
            "tool": "write_file"
        }, ensure_ascii=False)
    except Exception as e:
        return json.dumps({
            "error": f"Failed to write file: {e}",
            "tool": "write_file"
        }, ensure_ascii=False)
