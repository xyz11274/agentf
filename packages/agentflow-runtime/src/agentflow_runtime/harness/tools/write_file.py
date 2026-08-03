"""write_file tool — write content to a file on the local filesystem.

ToolExecutor 迁移（Phase 3，ADR 20260724-tools-runtime-refactor.md）：
- 通过 create_executor() 抽象层路由，不再内部 if-else
- sandbox-mode 由 factory 读取 AF_SANDBOX_MODE 决定 Host/HTTP/e2b
- 宿主侧保留路径 security 检查（sandbox_endpoint 为空时）
- execute() 签名不变（skill_executor 兼容）
- append 模式通过 read-append-write 在 executor 层实现（HostExecutor 无原生 append 支持）
"""

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


def execute(tool_input: dict, sandbox_endpoint: str = None) -> str:
    """写文件。

    迁移后：通过 ToolExecutor 抽象层路由（create_executor），不再内部 if-else。
    保持签名不变（skill_executor.py 兼容）。

    Args:
        tool_input: {"file_path": str, "content": str, "append": bool}
        sandbox_endpoint: 由 daemon 注入的沙箱 HTTP endpoint。

    Returns:
        JSON 字符串——WriteResult 格式（success/error/bytes_written）
        加 tool 字段标识来源。
    """
    from agentflow_runtime.executors.factory import create_executor

    file_path = tool_input.get("file_path", "")
    content = tool_input.get("content", "")
    append = tool_input.get("append", False)

    if not file_path:
        return json.dumps({"error": "file_path is required", "tool": "write_file"},
                          ensure_ascii=False)
    if content is None:
        content = ""

    # 宿主侧路径 security 检查（sandbox_endpoint 为空时，沙箱侧由 server 自行检查）
    if not sandbox_endpoint:
        resolved = os.path.abspath(file_path)
        cwd = os.path.abspath(os.getcwd())
        if not resolved.startswith(cwd):
            return json.dumps({
                "error": f"Access denied: path '{file_path}' is outside the working directory '{cwd}'",
                "tool": "write_file"
            }, ensure_ascii=False)

    # 通过 Factory 获取 executor
    executor = create_executor(sandbox_endpoint=sandbox_endpoint)

    # append 模式：read-append-write（HostExecutor 无原生 append 支持）
    if append:
        existing = executor.read_file(file_path)
        if existing.content:
            content = existing.content + content

    result = executor.write_file(file_path, content)

    # 统一序列化（WriteResult.to_dict() + 补 tool 字段）
    data = result.to_dict()
    data["tool"] = "write_file"
    return json.dumps(data, ensure_ascii=False)
