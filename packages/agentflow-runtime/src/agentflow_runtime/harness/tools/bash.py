"""Bash tool — execute arbitrary shell commands and return output."""

import json
import os
import subprocess
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

# bash.py 被 skill_executor 动态加载，AGENT_PACKAGE_DIR 在加载前由
# _LocalToolRegistry 通过 AF_AGENT_PACKAGE_DIR 环境变量注入。
# 若未设置（直接导入测试场景），fallback 到 tools 目录的 parents[2]。
_DEFAULT_AGENT_DIR = str(Path(__file__).resolve().parents[2])


def _get_agent_package_dir() -> str:
    """返回当前 agent 包根目录（由 skill_executor 在加载前注入环境变量）。"""
    return os.environ.get("AF_AGENT_PACKAGE_DIR", _DEFAULT_AGENT_DIR)


def execute(tool_input: dict) -> str:
    command = tool_input.get("command", "")
    timeout = tool_input.get("timeout", 30)

    if not command.strip():
        return json.dumps({"error": "Empty command"}, ensure_ascii=False)

    try:
        result = subprocess.run(
            command,
            shell=True,
            capture_output=True,
            text=True,
            timeout=timeout,
            cwd=_get_agent_package_dir(),
        )
        return json.dumps(
            {
                "stdout": result.stdout,
                "stderr": result.stderr,
                "exit_code": result.returncode,
                "tool": "bash",
            },
            ensure_ascii=False,
        )
    except subprocess.TimeoutExpired:
        return json.dumps(
            {
                "error": f"Command timed out after {timeout}s",
                "tool": "bash",
            },
            ensure_ascii=False,
        )
    except Exception as e:
        return json.dumps(
            {
                "error": str(e),
                "tool": "bash",
            },
            ensure_ascii=False,
        )
