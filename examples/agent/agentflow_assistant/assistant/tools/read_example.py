"""read_example tool — read an existing agent example for user reference."""

import json
import os


TOOL_NAME = "read_example"
TOOL_DESCRIPTION = "Read an existing agent example from the examples/agent/ directory for reference"
TOOL_PARAMS = {
    "type": "object",
    "properties": {
        "agent_name": {
            "type": "string",
            "description": "Name of the agent directory to read (e.g., 'harness_agent', 'basic_agent', 'multi_session_chat')"
        },
        "file_path": {
            "type": "string",
            "description": "Optional specific file to read within the agent (e.g., 'agent.json', 'package.json'). If omitted, lists the directory."
        }
    },
    "required": ["agent_name"]
}


EXAMPLES_DIR = os.path.normpath(os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    "..", "..", ".."
))


def _read_file_safe(path: str, max_chars: int = 5000) -> str:
    """Read a file safely, returning content or error message."""
    if not os.path.isfile(path):
        return json.dumps({"error": f"File not found: {path}"}, ensure_ascii=False)
    try:
        with open(path, encoding="utf-8") as f:
            content = f.read(max_chars)
        return content
    except Exception as e:
        return json.dumps({"error": f"Failed to read file: {e}"}, ensure_ascii=False)


def execute(tool_input: dict) -> dict:
    agent_name = tool_input.get("agent_name", "")
    file_path = tool_input.get("file_path", "")

    if not agent_name:
        return json.dumps({"error": "agent_name is required", "tool": "read_example"},
                          ensure_ascii=False)

    agent_dir = os.path.join(EXAMPLES_DIR, agent_name)

    if not os.path.isdir(agent_dir):
        # Try listing available agents
        available = []
        if os.path.isdir(EXAMPLES_DIR):
            available = [d for d in os.listdir(EXAMPLES_DIR)
                        if os.path.isdir(os.path.join(EXAMPLES_DIR, d))]
        return json.dumps({
            "error": f"Agent '{agent_name}' not found in examples/agent/",
            "available_agents": available,
            "tool": "read_example",
        }, ensure_ascii=False)

    if file_path:
        # Read specific file
        full_path = os.path.join(agent_dir, file_path)
        # Security: ensure the resolved path is within the agent directory
        resolved = os.path.normpath(full_path)
        if not resolved.startswith(os.path.normpath(agent_dir)):
            return json.dumps({
                "error": f"Access denied: path '{file_path}' is outside the agent directory",
                "tool": "read_example",
            }, ensure_ascii=False)
        content = _read_file_safe(resolved)
        return json.dumps({
            "agent_name": agent_name,
            "file_path": file_path,
            "content": content,
            "tool": "read_example",
        }, ensure_ascii=False)
    else:
        # List directory structure
        structure = []
        for root, dirs, files in os.walk(agent_dir):
            dirs[:] = [d for d in dirs if not d.startswith(".") and d != "__pycache__"]
            rel_path = os.path.relpath(root, agent_dir)
            prefix = "" if rel_path == "." else rel_path + "/"
            for f in files:
                if not f.startswith("."):
                    structure.append(prefix + f)

        return json.dumps({
            "agent_name": agent_name,
            "directory": agent_dir,
            "structure": structure,
            "tool": "read_example",
        }, ensure_ascii=False)
