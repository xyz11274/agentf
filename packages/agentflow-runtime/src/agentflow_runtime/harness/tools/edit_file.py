"""edit_file tool — find and replace a string in a file.

ToolExecutor 迁移（Phase 3，ADR 20260724-tools-runtime-refactor.md）：
- 通过 create_executor() 抽象层路由，不再内部 if-else
- sandbox-mode 由 factory 读取 AF_SANDBOX_MODE 决定 Host/HTTP/E2B
- 宿主侧保留路径 security 检查（sandbox_endpoint 为空时）
- execute() 签名不变（skill_executor 兼容）
"""

import json
import os

TOOL_NAME = "edit_file"
TOOL_DESCRIPTION = "Find and replace a string in a file. By default replaces the first unique occurrence; set replace_all to replace every occurrence."
TOOL_PARAMS = {
    "type": "object",
    "properties": {
        "file_path": {
            "type": "string",
            "description": "Absolute or relative path to the file to edit"
        },
        "old_str": {
            "type": "string",
            "description": "Exact text to find (must match EXACTLY including whitespace and indentation)"
        },
        "new_str": {
            "type": "string",
            "description": "Replacement text"
        },
        "replace_all": {
            "type": "boolean",
            "description": "If true, replace ALL occurrences (default false = require a unique match)",
            "default": False
        }
    },
    "required": ["file_path", "old_str", "new_str"]
}


def execute(tool_input: dict, sandbox_endpoint: str = None) -> str:
    """精确替换文件中的字符串。

    迁移后：通过 ToolExecutor 抽象层路由（create_executor），不再内部 if-else。
    保持签名不变（skill_executor.py 兼容）。

    Args:
        tool_input: {"file_path": str, "old_str": str, "new_str": str, "replace_all": bool}
        sandbox_endpoint: 由 daemon 注入的沙箱 HTTP endpoint。

    Returns:
        JSON 字符串——ExecResult 格式（exit_code/stdout/stderr/error/truncated）
        加 tool 和 success 字段（success 由 exit_code 派生，兼容旧格式）。
    """
    from agentflow_runtime.executors.factory import create_executor

    file_path = tool_input.get("file_path", "")
    old_str = tool_input.get("old_str", "")
    new_str = tool_input.get("new_str", "")
    replace_all = tool_input.get("replace_all", False)

    if not file_path:
        return json.dumps({"error": "file_path is required", "tool": "edit_file"},
                          ensure_ascii=False)
    if not old_str:
        return json.dumps({"error": "old_str is required", "tool": "edit_file"},
                          ensure_ascii=False)

    # 宿主侧路径 security 检查（sandbox_endpoint 为空时，沙箱侧由 server 自行检查）
    if not sandbox_endpoint:
        resolved = os.path.abspath(file_path)
        cwd = os.path.abspath(os.getcwd())
        if not resolved.startswith(cwd):
            return json.dumps({
                "error": f"Access denied: path '{file_path}' is outside the working directory '{cwd}'",
                "tool": "edit_file"
            }, ensure_ascii=False)

    # 通过 Factory 获取 executor
    executor = create_executor(sandbox_endpoint=sandbox_endpoint)
    result = executor.edit_file(file_path, old_str, new_str, replace_all)

    # 统一序列化（ExecResult.to_dict() + 补 tool 和 success 字段）
    data = result.to_dict()
    data["tool"] = "edit_file"
    data["success"] = result.exit_code == 0
    return json.dumps(data, ensure_ascii=False)
