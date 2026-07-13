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

# Appended to system prompt: instructs the LLM to stop retrying tools
# that keep failing (network error / API down) and return what it has.
SKILL_NETWORK_FAILURE_PROMPT = (
    "注意：如果某个工具连续两次返回网络错误或连接失败，"
    "说明该外部服务当前不可用。不要反复重试同一个工具。"
    "请基于已有信息输出结果，并在 skill_result 中说明哪些信息因服务不可用未能获取。"
)

# Instructs the LLM not to retry downloading network resources that fail,
# and to return the error info directly to the user instead.
SKILL_NETWORK_RESOURCE_PROMPT = (
    "注意：在尝试下载图片、文件等网络资源时，如果访问失败（如 404、连接超时、"
    "服务器拒绝），不要重试下载。直接返回错误信息告知用户，"
    "说明哪个资源无法获取及原因。"
)

# Instructs the LLM that generated artifact files (images, HTML, reports) are
# automatically collected from the output and published by the platform.
# The LLM should NOT attempt to start its own HTTP server or file service.
SKILL_ARTIFACT_AUTO_COLLECT_PROMPT = (
    "注意：你生成的所有产物文件（图片、HTML 报告、PPT 等）都会被系统自动收集"
    "并通过平台的工件发布机制展示给用户。"
    "你只需要调用相关工具生成产物，不需要自己启动 HTTP 服务器或文件服务。"
)
