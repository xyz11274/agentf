"""list_skills tool — lists available skills from harness/skills/."""

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
    global _SKILLS_DIR
    if _SKILLS_DIR is not None:
        return _SKILLS_DIR
    tools_dir = os.path.dirname(os.path.abspath(__file__))
    harness_dir = os.path.dirname(tools_dir)
    _SKILLS_DIR = os.path.join(harness_dir, "skills")
    return _SKILLS_DIR


def execute(tool_input: dict) -> str:
    skills_dir = _get_skills_dir()
    if not os.path.isdir(skills_dir):
        return json.dumps({"skills": [], "error": "skills directory not found"})

    skills = []
    for entry in sorted(os.listdir(skills_dir)):
        skill_md = os.path.join(skills_dir, entry, "SKILL.md")
        if os.path.isfile(skill_md):
            skills.append({"name": entry, "path": skill_md})
    return json.dumps({"skills": skills}, ensure_ascii=False)
