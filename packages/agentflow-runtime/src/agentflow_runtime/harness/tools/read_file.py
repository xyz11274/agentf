"""read_file tool — read a file from the local filesystem."""

import json
import os


TOOL_NAME = "read_file"
TOOL_DESCRIPTION = "Read the contents of a file from the local filesystem. Use with max_chars to avoid reading overly large files."
TOOL_PARAMS = {
    "type": "object",
    "properties": {
        "file_path": {
            "type": "string",
            "description": "Absolute or relative path to the file to read"
        },
        "max_chars": {
            "type": "integer",
            "description": "Maximum number of characters to return (default 10000, use -1 for no limit)",
            "default": 10000
        },
        "offset": {
            "type": "integer",
            "description": "Starting line number (1-based, default 1)",
            "default": 1
        },
        "limit": {
            "type": "integer",
            "description": "Max lines to return from offset (default -1 = all)",
            "default": -1
        }
    },
    "required": ["file_path"]
}


def execute(tool_input: dict) -> str:
    file_path = tool_input.get("file_path", "")
    max_chars = tool_input.get("max_chars", 10000)
    offset = tool_input.get("offset", 1)
    limit = tool_input.get("limit", -1)

    if not file_path:
        return json.dumps({"error": "file_path is required", "tool": "read_file"},
                          ensure_ascii=False)

    # Resolve relative paths against CWD
    resolved = os.path.abspath(file_path)

    # Security: reject paths outside the project
    cwd = os.path.abspath(os.getcwd())
    if not resolved.startswith(cwd):
        return json.dumps({
            "error": f"Access denied: path '{file_path}' is outside the working directory '{cwd}'",
            "tool": "read_file"
        }, ensure_ascii=False)

    if not os.path.isfile(resolved):
        return json.dumps({
            "error": f"File not found: {file_path}",
            "tool": "read_file"
        }, ensure_ascii=False)

    try:
        with open(resolved, encoding="utf-8", errors="replace") as f:
            if offset > 1 or limit != -1:
                # Line-based read
                lines = f.readlines()
                start = max(0, offset - 1)
                if limit > 0:
                    selected = lines[start:start + limit]
                else:
                    selected = lines[start:]
                content = "".join(selected)
                total_lines = len(lines)
            else:
                content = f.read()
                total_lines = content.count("\n") + 1

        if max_chars > 0 and len(content) > max_chars:
            content = content[:max_chars] + f"\n... (truncated at {max_chars} chars)"

        return json.dumps({
            "content": content,
            "char_count": len(content),
            "line_count": total_lines,
            "file_path": resolved,
            "tool": "read_file",
        }, ensure_ascii=False)

    except PermissionError:
        return json.dumps({
            "error": f"Permission denied: {file_path}",
            "tool": "read_file"
        }, ensure_ascii=False)
    except Exception as e:
        return json.dumps({
            "error": f"Failed to read file: {e}",
            "tool": "read_file"
        }, ensure_ascii=False)
