"""Bash tool — execute arbitrary shell commands and return output."""

import json
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

# bash.py 位于 <agent_package>/harness/tools/bash.py
# 锁定 cwd 到 agent_package 根，与 extract_artifact_paths 的 AGENT_PACKAGE_DIR
# 基准对齐。若不锁定，subprocess 继承的 cwd（框架 worker 设置）可能与
# extract_artifact_paths 的解析基准不一致，导致相对路径（如 ./assets/xx.png）
# 写入位置与 URL 解析位置漂移。
_AGENT_PACKAGE_DIR = str(Path(__file__).resolve().parents[2])


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
            cwd=_AGENT_PACKAGE_DIR,
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
