"""list_skills tool — lists available skills from harness/skills/.

ToolExecutor 迁移（Phase 3，ADR 20260724-tools-runtime-refactor.md）：
- skills 目录始终在宿主机上（不在沙箱内），无需通过 executor 路由
- 移除 sandbox HTTP POST 路径（从未被有效使用——skills 从不在沙箱内）
- 保留 _get_skills_dir() 和宿主机 os.walk() 逻辑
- execute() 签名不变（skill_executor 兼容，sandbox_endpoint 参数保留但忽略）
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


def execute(tool_input: dict, sandbox_endpoint: str = None) -> str:
    """列出可用技能。

    skills 目录始终在宿主机上，不通过 executor 路由。
    sandbox_endpoint 参数保留以兼容签名，但被忽略。

    Args:
        tool_input: {}（无参数）
        sandbox_endpoint: 保留兼容，忽略（skills 在宿主机上）。

    Returns:
        JSON 字符串——{"skills": [{"name": "...", "path": "..."}]}
    """
    skills_dir = _get_skills_dir()
    if not os.path.isdir(skills_dir):
        return json.dumps({"skills": [], "error": "skills directory not found"})

    skills = []
    for root, dirs, files in os.walk(skills_dir):
        dirs[:] = [d for d in dirs if not d.startswith(".")]
        if "SKILL.md" in files:
            rel = os.path.relpath(root, skills_dir)
            skill_md = os.path.join(root, "SKILL.md")
            skills.append({"name": rel, "path": skill_md})
    return json.dumps({"skills": skills}, ensure_ascii=False)
