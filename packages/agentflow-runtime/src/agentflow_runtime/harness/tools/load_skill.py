"""load_skill tool — reads a SKILL.md from harness/skills/{name}/."""

import json
import os

TOOL_NAME = "load_skill"
TOOL_DESCRIPTION = "Load a skill's SKILL.md into context by skill name"
TOOL_PARAMS = {
    "type": "object",
    "properties": {
        "skill_name": {
            "type": "string",
            "description": "Name of the skill to load"
        }
    },
    "required": ["skill_name"]
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
    skill_name = tool_input.get("skill_name", "")
    if not skill_name:
        return json.dumps({"error": "skill_name is required"})

    skill_path = os.path.join(_get_skills_dir(), skill_name, "SKILL.md")
    if not os.path.isfile(skill_path):
        return json.dumps({"error": f"skill '{skill_name}' not found"})

    with open(skill_path, encoding="utf-8") as f:
        content = f.read()
    return json.dumps({"skill_name": skill_name, "content": content}, ensure_ascii=False)
