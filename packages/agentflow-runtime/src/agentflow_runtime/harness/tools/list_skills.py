"""list_skills tool — lists available skills from harness/skills/.

Discovers skills by recursively searching for directories containing
``SKILL.md``.  Nested paths (e.g. ``minimax_outfit_skills/forge-image-gen``)
are reported as the skill name, preserving the filesystem hierarchy.
"""

import json
import os

TOOL_NAME = "list_skills"
TOOL_DESCRIPTION = "List all available skills that can be loaded into context"
TOOL_PARAMS = {
    "type": "object",
    "properties": {},
    "required": []
}

_SKILLS_DIR = None


def _get_skills_dir():
    """返回 skill 目录（优先使用 AF_AGENT_PACKAGE_DIR 环境变量）。"""
    global _SKILLS_DIR
    if _SKILLS_DIR is not None:
        return _SKILLS_DIR
    agent_dir = os.environ.get("AF_AGENT_PACKAGE_DIR", "")
    if agent_dir:
        _SKILLS_DIR = os.path.join(agent_dir, "harness", "skills")
    else:
        # Fallback: 相对于 tools 目录（兼容直接导入测试）
        tools_dir = os.path.dirname(os.path.abspath(__file__))
        harness_dir = os.path.dirname(tools_dir)
        _SKILLS_DIR = os.path.join(harness_dir, "skills")
    return _SKILLS_DIR


def execute(tool_input: dict) -> str:
    skills_dir = _get_skills_dir()
    if not os.path.isdir(skills_dir):
        return json.dumps({"skills": [], "error": "skills directory not found"})

    skills = []
    # Recursively walk the skills directory to find SKILL.md files.
    # This supports nested paths (e.g. "minimax_outfit_skills/forge-image-gen").
    for root, dirs, files in os.walk(skills_dir):
        # Skip hidden directories
        dirs[:] = [d for d in dirs if not d.startswith(".")]
        if "SKILL.md" in files:
            # Use the relative path from skills_dir as the skill name
            rel = os.path.relpath(root, skills_dir)
            skill_md = os.path.join(root, "SKILL.md")
            skills.append({"name": rel, "path": skill_md})
    return json.dumps({"skills": skills}, ensure_ascii=False)
