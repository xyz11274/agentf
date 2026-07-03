"""validate_package tool — validate a generated agent package structure."""

import json
import os


TOOL_NAME = "validate_package"
TOOL_DESCRIPTION = "Validate a generated agent package in examples/agent/ to ensure it has all required files and correct structure"
TOOL_PARAMS = {
    "type": "object",
    "properties": {
        "agent_name": {
            "type": "string",
            "description": "Name of the agent directory to validate (e.g., 'my_agent')"
        }
    },
    "required": ["agent_name"]
}


EXAMPLES_DIR = os.path.normpath(os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    "..", "..", ".."
))

# Required files for a valid agent package
REQUIRED_FILES = {
    "package.json": "Package metadata",
    "agent.json": "Task definition (dispatcher + coordinator + skill)",
}

# Optional but recommended files
OPTIONAL_FILES = {
    "README.md": "Documentation",
    "README.md": "Documentation",
    ".env.example": "Environment variable template",
}


def _check_json(filepath: str) -> tuple:
    """Check if a file is valid JSON. Returns (valid: bool, error: str)."""
    try:
        with open(filepath, encoding="utf-8") as f:
            json.load(f)
        return True, ""
    except json.JSONDecodeError as e:
        return False, f"Invalid JSON: {e}"
    except Exception as e:
        return False, f"Read error: {e}"


def execute(tool_input: dict) -> str:
    agent_name = tool_input.get("agent_name", "")

    if not agent_name:
        return json.dumps({"error": "agent_name is required", "tool": "validate_package"},
                          ensure_ascii=False)

    agent_dir = os.path.join(EXAMPLES_DIR, agent_name)

    if not os.path.isdir(agent_dir):
        return json.dumps({
            "error": f"Agent directory not found: {agent_dir}",
            "valid": False,
            "tool": "validate_package",
        }, ensure_ascii=False)

    result = {
        "agent_name": agent_name,
        "path": agent_dir,
        "valid": True,
        "files": {},
        "missing_required": [],
        "present_optional": [],
        "issues": [],
        "tool": "validate_package",
    }

    # Check required files
    for fname, desc in REQUIRED_FILES.items():
        fpath = os.path.join(agent_dir, fname)
        exists = os.path.isfile(fpath)
        result["files"][fname] = {
            "exists": exists,
            "description": desc,
        }
        if not exists:
            result["missing_required"].append(fname)
            result["valid"] = False
        elif fname.endswith(".json"):
            valid_json, err = _check_json(fpath)
            if not valid_json:
                result["issues"].append(f"{fname}: {err}")
                result["valid"] = False

    # Check for workflow files (referenced in package.json)
    pkg_path = os.path.join(agent_dir, "package.json")
    if os.path.isfile(pkg_path):
        try:
            with open(pkg_path, encoding="utf-8") as f:
                pkg = json.load(f)
            workflows = pkg.get("workflows", [])
            for wf in workflows:
                wf_path = os.path.join(agent_dir, wf)
                wf_exists = os.path.isfile(wf_path)
                result["files"][wf] = {
                    "exists": wf_exists,
                    "description": f"Workflow referenced in package.json",
                }
                if not wf_exists:
                    result["issues"].append(f"Workflow '{wf}' referenced in package.json but not found")
                    result["valid"] = False
        except Exception as e:
            result["issues"].append(f"package.json parse error: {e}")
            result["valid"] = False

    # Check for nodes directory
    nodes_dir = os.path.join(agent_dir, "nodes")
    if os.path.isdir(nodes_dir):
        node_files = [f for f in os.listdir(nodes_dir)
                     if f.endswith(".py") and not f.startswith("_")]
        result["files"]["nodes/"] = {
            "exists": True,
            "node_count": len(node_files),
            "nodes": node_files,
        }
    else:
        result["files"]["nodes/"] = {
            "exists": False,
            "note": "No nodes directory (optional for non-Python agents)",
        }

    # Count total files
    all_files = []
    for root, dirs, files in os.walk(agent_dir):
        dirs[:] = [d for d in dirs if not d.startswith(".") and d != "__pycache__"]
        for f in files:
            if not f.startswith("."):
                rel = os.path.relpath(os.path.join(root, f), agent_dir)
                all_files.append(rel)
    result["total_files"] = len(all_files)
    result["file_list"] = sorted(all_files)

    return json.dumps(result, ensure_ascii=False)
