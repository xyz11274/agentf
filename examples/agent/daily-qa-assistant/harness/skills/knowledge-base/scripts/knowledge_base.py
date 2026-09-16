#!/usr/bin/env python3
"""knowledge_base tool — 知识库检索（mock，固定返回示例条目）。

skill-scoped tool，由 skill_executor 动态加载。
模块契约：TOOL_NAME / TOOL_DESCRIPTION / TOOL_PARAMS / execute(args)。
"""

TOOL_NAME = "knowledge_base"
TOOL_DESCRIPTION = (
    "Knowledge base retrieval mock: returns fixed sample entries for any query. "
    "Always succeeds."
)
TOOL_PARAMS = {
    "type": "object",
    "properties": {
        "query": {"type": "string", "description": "检索问题"},
        "top_k": {"type": "integer", "minimum": 1, "maximum": 5, "description": "返回条数（默认 2）"},
    },
    "required": ["query"],
    "additionalProperties": False,
}

_ENTRIES = [
    {"title": "示例条目一", "content": "这是知识库示例内容：请在 OA 系统提交申请，直属上级审批后生效。", "score": 0.92},
    {"title": "示例条目二", "content": "这是知识库示例内容：详细流程参见内部文档第 3 章。", "score": 0.85},
]


def execute(args, sandbox_endpoint=None):
    query = (args or {}).get("query")
    if not query:
        return {"status": "failed", "error": "query is required"}
    top_k = int((args or {}).get("top_k") or 2)
    entries = _ENTRIES[:max(1, min(top_k, len(_ENTRIES)))]
    return {
        "status": "succeeded",
        "query": query,
        "entries": entries,
        "message": f"命中 {len(entries)} 条知识库条目（示例数据）",
        "mock": True,
    }
