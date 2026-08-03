"""read_file tool — read a file from the local filesystem.

ToolExecutor 迁移（Phase 3，ADR 20260724-tools-runtime-refactor.md）：
- 通过 create_executor() 抽象层路由，不再内部 if-else
- sandbox-mode 由 factory 读取 AF_SANDBOX_MODE 决定 Host/HTTP/e2b
- 宿主侧保留路径 security 检查（sandbox_endpoint 为空时）
- execute() 签名不变（skill_executor 兼容）
"""

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


def execute(tool_input: dict, sandbox_endpoint: str = None) -> str:
    """读取文件内容。

    迁移后：通过 ToolExecutor 抽象层路由（create_executor），不再内部 if-else。
    保持签名不变（skill_executor.py 兼容）。

    Args:
        tool_input: {"file_path": str, "max_chars": int, "offset": int, "limit": int}
        sandbox_endpoint: 由 daemon 注入的沙箱 HTTP endpoint。
            传递给 create_executor 作为 endpoint override。

    Returns:
        JSON 字符串——FileContent 格式（content/error/total_lines）
        加 tool 字段标识来源。
    """
    from agentflow_runtime.executors.factory import create_executor

    file_path = tool_input.get("file_path", "")
    max_chars = tool_input.get("max_chars", 10000)
    offset = tool_input.get("offset", 1)
    limit = tool_input.get("limit", -1)

    if not file_path:
        return json.dumps({"error": "file_path is required", "tool": "read_file"},
                          ensure_ascii=False)

    # 宿主侧路径 security 检查（sandbox_endpoint 为空时，沙箱侧由 server 自行检查）
    if not sandbox_endpoint:
        resolved = os.path.abspath(file_path)
        cwd = os.path.abspath(os.getcwd())
        if not resolved.startswith(cwd):
            return json.dumps({
                "error": f"Access denied: path '{file_path}' is outside the working directory '{cwd}'",
                "tool": "read_file"
            }, ensure_ascii=False)

    # 通过 Factory 获取 executor（替代原来的 if-else 分支）
    executor = create_executor(sandbox_endpoint=sandbox_endpoint)
    result = executor.read_file(file_path, offset if offset > 0 else 0, limit if limit > 0 else 0)

    # 统一序列化（FileContent.to_dict() + 补 tool 字段）
    data = result.to_dict()
    data["tool"] = "read_file"

    # max_chars 截断（宿主侧逻辑，保留不变）
    if max_chars > 0 and data.get("content") and len(data["content"]) > max_chars:
        data["content"] = data["content"][:max_chars] + f"\n... (truncated at {max_chars} chars)"

    return json.dumps(data, ensure_ascii=False)
