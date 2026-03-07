#!/bin/sh
set -eu

python3 - <<'PY'
import json

catalog = [
    {
        "skill": "skill_search",
        "workflow_id": "skill-search-workflow",
        "title": "搜索检索",
        "description": "检索最近信息、事实线索和外部背景。",
    },
    {
        "skill": "skill_summary",
        "workflow_id": "skill-summary-workflow",
        "title": "总结归纳",
        "description": "整理已有信息并输出结构化总结。",
    },
    {
        "skill": "skill_date",
        "workflow_id": "skill-date-workflow",
        "title": "日期时间",
        "description": "处理日期、星期、时间安排等问题。",
    },
    {
        "skill": "skill_delegate",
        "workflow_id": "skill-delegate-workflow",
        "title": "委托规划",
        "description": "生成步骤方案、计划和执行建议。",
    },
]

print(
    json.dumps(
        {"skill_catalog": json.dumps(catalog, ensure_ascii=False, separators=(",", ":"))},
        ensure_ascii=False,
    ))
PY
