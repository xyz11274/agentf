"""Bash tool — execute arbitrary shell commands and return output.

ToolExecutor 迁移（Phase 3 先行验证，ADR 20260724-tools-runtime-refactor.md）：
- 通过 create_executor() 抽象层路由，不再内部 if-else
- sandbox-mode 由 factory 读取 AF_SANDBOX_MODE 决定 Host/HTTP/e2b
- 宿主侧保留第一道防线：危険命令检查（B2）
- execute() 签名不变（skill_executor 兼容）
"""

import json
import os
from pathlib import Path

TOOL_NAME = "bash"
TOOL_DESCRIPTION = "Execute a shell command on the local machine and return its output."
TOOL_PARAMS = {
    "type": "object",
    "properties": {
        "command": {"type": "string", "description": "Shell command to execute"},
        "timeout": {
            "type": "integer",
            "description": "Timeout in seconds (default 30)",
            "default": 30,
        },
    },
    "required": ["command"],
}

# 危険命令前缀（宿主侧第一道防线，B2；沙箱侧 sandbox_http_server.py 保留第二道防线）
_DANGEROUS_PREFIXES = (
    "rm -rf /", "sudo ", "sudo\t", ":(){", "fork bomb",
    "mkfs.", "dd if=/dev/zero of=/dev/sd", "shutdown", "reboot",
)

# bash.py 被 skill_executor 动态加载，AGENT_PACKAGE_DIR 在加载前由
# _LocalToolRegistry 通过 AF_AGENT_PACKAGE_DIR 环境变量注入。
# 若未设置（直接导入测试场景），fallback 到 tools 目录的 parents[2]。
_DEFAULT_AGENT_DIR = str(Path(__file__).resolve().parents[2])


def _get_agent_package_dir() -> str:
    """返回当前 agent 包根目录（由 skill_executor 在加载前注入环境变量）。"""
    return os.environ.get("AF_AGENT_PACKAGE_DIR", _DEFAULT_AGENT_DIR)


def _is_dangerous(command: str) -> bool:
    """检查命令是否含危険前缀（B2 宿主侧第一道防线）。"""
    stripped = command.strip()
    return any(stripped.startswith(p) or p in stripped for p in _DANGEROUS_PREFIXES)


def execute(tool_input: dict, sandbox_endpoint: str = None) -> str:
    """执行 shell 命令。

    迁移后：通过 ToolExecutor 抽象层路由（create_executor），不再内部 if-else。
    保持签名不变（skill_executor.py L212 兼容）。

    Args:
        tool_input: {"command": str, "timeout": int, "cwd": str (optional)}
        sandbox_endpoint: 由 daemon 注入的沙箱 HTTP endpoint（B16）。
            传递给 create_executor 作为 endpoint override。

    Returns:
        JSON 字符串——ExecResult 格式（stdout/stderr/exit_code/error/truncated）
        加 tool 字段标识来源。
    """
    from agentflow_runtime.executors.factory import create_executor

    command = tool_input.get("command", "")
    timeout = tool_input.get("timeout", 30)
    # cwd override（webshell 跨命令 cwd 延续）：外部传入优先，缺省回落到 agent package dir
    # 不改默认语义——非 webshell 路径（skill_executor 等）不传 cwd，行为不变
    cwd = tool_input.get("cwd")
    workdir = cwd if cwd else _get_agent_package_dir()

    if not command.strip():
        return json.dumps({"error": "Empty command"}, ensure_ascii=False)

    # 宿主侧第一道防线：危険命令检查（B2，保留不迁移）
    if _is_dangerous(command):
        return json.dumps(
            {"error": f"Dangerous command rejected: {command}", "tool": "bash"},
            ensure_ascii=False,
        )

    # 通过 Factory 获取 executor（替代原来的 if-else 分支）
    executor = create_executor(sandbox_endpoint=sandbox_endpoint)
    result = executor.exec_command(command, timeout, workdir=workdir)
    # 统一序列化（ExecResult.to_dict() + 补 tool 字段）
    data = result.to_dict()
    data["tool"] = "bash"
    return json.dumps(data, ensure_ascii=False)
