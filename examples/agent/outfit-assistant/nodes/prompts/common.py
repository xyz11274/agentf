"""
Common prompts — shared message templates used by multiple nodes
(coordinator, skill_executor, round_reminder).
"""

# ---------------------------------------------------------------------------
# LLM error / fallback responses
# ---------------------------------------------------------------------------

# Used when LLM call fails but skill produced artifacts
LLM_ERROR_WITH_ARTIFACTS = "文本整理暂时失败，已生成的产物仍可查看。"

# Used when LLM call fails with no artifacts to show
LLM_ERROR_NO_ARTIFACTS = "抱歉，本次模型调用失败，任务未能完成，请稍后重试。"

# ---------------------------------------------------------------------------
# Artifact-related UI templates
# ---------------------------------------------------------------------------

ARTIFACTS_SECTION_HEADER = "## 生成产物"
ARTIFACT_IMAGE_TEMPLATE = "![生成图片 {index}]({url})"
ARTIFACT_HTML_TEMPLATE = "[查看报告 {index}]({url})"
ARTIFACT_DOWNLOAD_TEMPLATE = "[📥 下载文件 {index}]({url})"
