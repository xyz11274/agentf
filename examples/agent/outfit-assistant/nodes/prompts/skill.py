"""
Skill prompts — system prompt and message templates for the skill executor
node (skill_executor.py).
"""

# ---------------------------------------------------------------------------
# System prompt fallbacks
# ---------------------------------------------------------------------------

# Used when no SKILL.md docs are loaded (fallback)
SKILL_FALLBACK_SYSTEM = "你是一个任务执行 agent。完成指派给你的任务。"

# Template for each skill's section in the system prompt
SKILL_SECTION_TEMPLATE = "## Skill: {name}\n\n{content}"

# Used when a SKILL.md file is not found
SKILL_NOT_FOUND_TEMPLATE = "[Skill '{name}' 文档未找到]"

# ---------------------------------------------------------------------------
# Recovery / error messages
# ---------------------------------------------------------------------------

# Injected when LLM output was truncated mid-tool-call
SKILL_RECOVERY_PROMPT = (
    "上一轮输出在工具参数完成前被截断。不要执行上一轮调用。"
    "请用更短内容重新生成完整、合法的 JSON 工具参数；"
    "较长正文可拆分成多次工具调用。"
)
