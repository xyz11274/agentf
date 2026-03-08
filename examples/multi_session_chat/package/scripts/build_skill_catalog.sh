#!/bin/sh
set -eu

python3 - <<'PY'
import json

catalog = [
    {
        "skill": "skill_search",
        "workflow_id": "skill-search-workflow",
        "title": "搜索检索",
        "description": "检索背景资料、新闻线索和外部信息。",
        "handles": ["搜索", "检索", "新闻", "热点", "资料背景"],
        "does_not_handle": ["实时天气", "未接入外部数据源的即时事实问答"],
        "abstain_when": "需要实时在线数据但当前示例未接入对应工具",
    },
    {
        "skill": "skill_summary",
        "workflow_id": "skill-summary-workflow",
        "title": "总结归纳",
        "description": "整理已有信息并输出结构化总结。",
        "handles": ["总结上条消息", "归纳已有结果", "复盘对话内容"],
        "does_not_handle": ["直接回答外部事实问题"],
        "abstain_when": "没有可总结的历史或中间结果",
    },
    {
        "skill": "skill_date",
        "workflow_id": "skill-date-workflow",
        "title": "日期时间",
        "description": "处理日期、星期、时间安排等问题。",
        "handles": ["几号", "星期几", "日期推算", "当前时间"],
        "does_not_handle": ["天气", "新闻", "泛化常识问答"],
        "abstain_when": "输入不包含明确日期时间语义",
    },
    {
        "skill": "skill_delegate",
        "workflow_id": "skill-delegate-workflow",
        "title": "委托规划",
        "description": "生成步骤方案、计划和执行建议。",
        "handles": ["计划", "方案", "任务拆解", "执行编排"],
        "does_not_handle": ["纯事实问答", "日期计算", "搜索结果本身"],
        "abstain_when": "请求并不需要计划或协调",
    },
]

print(
    json.dumps(
        {"skill_catalog": json.dumps(catalog, ensure_ascii=False, separators=(",", ":"))},
        ensure_ascii=False,
    ))
PY
