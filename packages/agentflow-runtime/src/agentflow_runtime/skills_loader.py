"""Skill MD loading and agent.md parsing.

Split from skill_executor.py (2026-08-14).  All public symbols are re-exported
by skill_executor so existing ``from agentflow_runtime.skill_executor import X``
call sites continue to work unchanged.

Design constraints:
  - AGENT_PACKAGE_DIR remains the single source of truth in skill_executor.
    This module reads it lazily via ``_se.AGENT_PACKAGE_DIR``.
  - _get_skills_base / _get_agent_package_dir live in skill_executor; this
    module calls them via lazy import.
  - _expand_env / RE_ENV_VAR live in tool_registry; parse_agent_md imports them
    from there.
  - No business logic / log format / protocol changes — pure structural split.
"""

import logging
import os
import sys

import yaml

from agentflow_runtime.tool_registry import _expand_env
from prompts import SKILL_NOT_FOUND_TEMPLATE

logger = logging.getLogger(__name__)


def _get_skills_base() -> str:
    """Return path to the skills base directory (delegates to skill_executor)."""
    from agentflow_runtime import skill_executor as _se
    return _se._get_skills_base()


def _get_agent_package_dir() -> str:
    """Return the agent package directory (delegates to skill_executor)."""
    from agentflow_runtime import skill_executor as _se
    return _se._get_agent_package_dir()


def parse_agent_md(path: str) -> tuple:
    if not os.path.isfile(path):
        return {}, ""
    with open(path, encoding="utf-8") as f:
        raw = f.read()
    parts = raw.split("---", 2)
    if len(parts) < 3:
        return {}, raw.strip()
    try:
        frontmatter = yaml.safe_load(parts[1]) or {}
        frontmatter = _expand_env(frontmatter)
    except Exception:
        frontmatter = {}
        print("[flow] WARN: agent.md frontmatter parse failed", file=sys.stderr)
    return frontmatter, parts[2].strip()


# ---------------------------------------------------------------------------
# SKILL.md loading
# ---------------------------------------------------------------------------

def load_skill_md(skill_name: str) -> str | None:
    """Read harness/skills/{skill_name}/SKILL.md and replace AgentFoundry template vars.

    AgentFoundry materializes placeholders like ${SKILL_DIR} and ${SKILLS_DIR}
    at mount time.  Our V3 coordinator doesn't have a materialization step,
    so we substitute them here with the actual filesystem paths.
    """
    path = os.path.join(_get_skills_base(), skill_name, "SKILL.md")
    if not os.path.isfile(path):
        return None
    with open(path, encoding="utf-8") as f:
        content = f.read()
    # Replace AgentFoundry template variables
    # sandbox_mode=e2b 时 SKILL_DIR 必须用沙箱内绝对路径而非宿主路径——否则 LLM
    # 拼命令含宿主路径 /Users/xxx/...，e2b.py exec_command 翻译回沙箱路径后
    # 目标在沙箱里也不存在（agent 包上传时按 harness/skills 结构落盘到
    # /home/user/workspace/harness/skills/...）。沙箱透明性要求 LLM 看到的是
    # 沙箱内它能直接用的路径，而不是宿主路径再翻译。
    skills_base = _get_skills_base()
    skill_dir = os.path.join(skills_base, skill_name)
    sandbox_mode = os.environ.get("AF_SANDBOX_MODE", "")
    if sandbox_mode == "e2b":
        # SANDBOX_WORKSPACE = /home/user/workspace（e2b.py 同步常量）
        # skills_base 形如 /Users/xxx/.../outfit-assistant/harness/skills
        # → 沙箱内 /home/user/workspace/harness/skills
        sandbox_workspace = os.environ.get(
            "AF_SANDBOX_WORKSPACE", "/home/user/workspace"
        )
        agent_pkg_dir = _get_agent_package_dir()
        if agent_pkg_dir and skills_base.startswith(agent_pkg_dir):
            skills_base = sandbox_workspace + skills_base[len(agent_pkg_dir):]
            skill_dir = os.path.join(skills_base, skill_name)
    content = content.replace("${SKILL_DIR}", skill_dir)
    content = content.replace("${SKILLS_DIR}", skills_base)
    return content


def load_skills_from_list(skill_names: list) -> dict:
    """Batch load SKILL.md files, returns {name: content}"""
    result = {}
    for name in skill_names:
        content = load_skill_md(name)
        result[name] = content if content else SKILL_NOT_FOUND_TEMPLATE.format(name=name)
    return result
