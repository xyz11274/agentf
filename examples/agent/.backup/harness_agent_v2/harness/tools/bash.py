"""Bash tool — execute arbitrary shell commands and return output."""

import json
import shlex
import subprocess

TOOL_NAME = "bash"
TOOL_DESCRIPTION = "Execute a shell command on the local machine and return its output."
TOOL_PARAMS = {
    "type": "object",
    "properties": {
        "command": {
            "type": "string",
            "description": "Shell command to execute"
        },
        "timeout": {
            "type": "integer",
            "description": "Timeout in seconds (default 30)",
            "default": 30
        }
    },
    "required": ["command"]
}


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
        )
        return json.dumps({
            "stdout": result.stdout,
            "stderr": result.stderr,
            "exit_code": result.returncode,
            "tool": "bash",
        }, ensure_ascii=False)
    except subprocess.TimeoutExpired:
        return json.dumps({
            "error": f"Command timed out after {timeout}s",
            "tool": "bash",
        }, ensure_ascii=False)
    except Exception as e:
        return json.dumps({
            "error": str(e),
            "tool": "bash",
        }, ensure_ascii=False)
