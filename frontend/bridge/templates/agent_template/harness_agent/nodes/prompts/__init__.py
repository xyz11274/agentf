"""
Prompts — centralized prompt management for harness agent v3.

All hardcoded system prompts, user-facing message templates, tool definitions,
and fallback texts live here instead of being scattered across node scripts.

Usage::

    from prompts import PROTOCOL_TEMPLATE, SKILL_FALLBACK_SYSTEM
    messages = [{"role": "system", "content": PROTOCOL_TEMPLATE.format(available_skills="...")}]
"""

from prompts.coordinator import (
    PROTOCOL_TEMPLATE,
    COORDINATOR_FALLBACK_SYSTEM,
    RUN_SKILLS_TOOL_DEFINITION,
    SKILL_COMPLETION_TEMPLATE,
    DECISION_FAILURE_PROMPT,
    DECISION_SUCCESS_PROMPT,
)

from prompts.common import (
    LLM_ERROR_WITH_ARTIFACTS,
    LLM_ERROR_NO_ARTIFACTS,
    ARTIFACTS_SECTION_HEADER,
    ARTIFACT_IMAGE_TEMPLATE,
    ARTIFACT_HTML_TEMPLATE,
)

from prompts.skill import (
    SKILL_FALLBACK_SYSTEM,
    SKILL_SECTION_TEMPLATE,
    SKILL_NOT_FOUND_TEMPLATE,
    SKILL_RECOVERY_PROMPT,
)

__all__ = [
    # coordinator
    "PROTOCOL_TEMPLATE",
    "COORDINATOR_FALLBACK_SYSTEM",
    "RUN_SKILLS_TOOL_DEFINITION",
    "SKILL_COMPLETION_TEMPLATE",
    "DECISION_FAILURE_PROMPT",
    "DECISION_SUCCESS_PROMPT",
    # common
    "LLM_ERROR_WITH_ARTIFACTS",
    "LLM_ERROR_NO_ARTIFACTS",
    "ARTIFACTS_SECTION_HEADER",
    "ARTIFACT_IMAGE_TEMPLATE",
    # skill
    "SKILL_FALLBACK_SYSTEM",
    "SKILL_SECTION_TEMPLATE",
    "SKILL_NOT_FOUND_TEMPLATE",
    "SKILL_RECOVERY_PROMPT",
]
