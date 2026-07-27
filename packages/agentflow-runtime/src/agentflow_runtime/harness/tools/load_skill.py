"""load_skill tool — reads a SKILL.md from harness/skills/{name}/.

ToolExecutor 迁移（Phase 3，ADR 20260724-tools-runtime-refactor.md）：
- skills 目录始终在宿主机上（不在沙箱内），无需通过 executor 路由
- 移除 sandbox HTTP POST 路径（从未被有效使用——skills 从不在沙箱内）
- 保留 _get_skills_dir() 和 _find_skill_path() 宿主机逻辑
- execute() 签名不变（skill_executor 兼容，sandbox_endpoint 参数保留但忽略）
"""

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


def _find_skill_path(skills_dir: str, skill_name: str) -> str | None:
    """Search for a skill directory recursively.

    Tries in order:
      1. Flat lookup: ``{skills_dir}/{skill_name}/SKILL.md``
      2. Recursive walk: find any subdirectory whose basename matches ``skill_name``
         and contains ``SKILL.md``.
    """
    # 1. Flat lookup
    direct = os.path.join(skills_dir, skill_name, "SKILL.md")
    if os.path.isfile(direct):
        return direct

    # 2. Recursive fallback — skills may be nested under subdirectories
    #    (e.g. minimax_outfit_skills/forge-image-gen/SKILL.md).
    for root, _dirs, files in os.walk(skills_dir):
        if "SKILL.md" in files and os.path.basename(root) == skill_name:
            return os.path.join(root, "SKILL.md")

    return None


def execute(tool_input: dict, sandbox_endpoint: str = None) -> str:
    """加载技能 SKILL.md。

    skills 目录始终在宿主机上，不通过 executor 路由。
    sandbox_endpoint 参数保留以兼容签名，但被忽略。

    Args:
        tool_input: {"skill_name": str}
        sandbox_endpoint: 保留兼容，忽略（skills 在宿主机上）。

    Returns:
        JSON 字符串——{"skill_name": "...", "content": "..."} 或 {"error": "..."}
    """
    skill_name = tool_input.get("skill_name", "")
    if not skill_name:
        return json.dumps({"error": "skill_name is required"})

    skills_dir = _get_skills_dir()
    skill_path = _find_skill_path(skills_dir, skill_name)
    if skill_path is None:
        return json.dumps({"error": f"skill '{skill_name}' not found"})

    with open(skill_path, encoding="utf-8") as f:
        content = f.read()
    return json.dumps({"skill_name": skill_name, "content": content}, ensure_ascii=False)
