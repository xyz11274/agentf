#!/usr/bin/env python3
import re
from typing import Dict


SUMMARY_INTENT = re.compile(r"总结|概括|摘要|提炼|复盘|整理|上条消息|上一条|总结一下|归纳")
SEARCH_INTENT = re.compile(r"搜索|检索|查询|查找|资料|背景|新闻|热点|信息|百科|来源|外部")
SEARCH_UNSUPPORTED = re.compile(r"天气|气温|降雨|降水|风力|台风|空气质量")
DATE_CORE = re.compile(r"几号|日期|时间|几点|几分|星期|周[一二三四五六日天]|本周|下周|上周|月|年|UTC")
DATE_RELATIVE = re.compile(r"今天|明天|后天|昨天|今晚|明早|现在")
DELEGATE_INTENT = re.compile(r"计划|方案|安排|执行|协调|分工|步骤|路线|委托|代理|拆解|排期")


def detect_summary_intent(text: str) -> bool:
    return bool(SUMMARY_INTENT.search((text or "").strip()))


def detect_search_intent(text: str) -> bool:
    value = (text or "").strip()
    return bool(SEARCH_INTENT.search(value)) and not bool(SEARCH_UNSUPPORTED.search(value))


def detect_date_intent(text: str) -> bool:
    value = (text or "").strip()
    has_relative = bool(DATE_RELATIVE.search(value))
    has_core = bool(DATE_CORE.search(value))
    return has_core or (has_relative and has_core)


def detect_delegate_intent(text: str) -> bool:
    return bool(DELEGATE_INTENT.search((text or "").strip()))


def assess_search_query(text: str) -> Dict:
    value = (text or "").strip()
    if not value:
        return {
            "can_handle": False,
            "status": "unsupported",
            "grounded": False,
            "confidence": 0.0,
            "reason": "缺少待检索的问题。",
        }
    if SEARCH_UNSUPPORTED.search(value):
        return {
            "can_handle": False,
            "status": "unsupported",
            "grounded": False,
            "confidence": 0.0,
            "reason": "当前搜索技能未接入实时天气等在线数据源。",
        }
    if detect_search_intent(value):
        return {
            "can_handle": True,
            "status": "limited",
            "grounded": False,
            "confidence": 0.72,
            "reason": "该问题需要检索/背景信息，但当前示例仅提供有限的检索建议能力。",
        }
    return {
        "can_handle": False,
        "status": "unsupported",
        "grounded": False,
        "confidence": 0.0,
        "reason": "该问题不属于搜索检索技能范围。",
    }


def assess_summary_query(text: str) -> Dict:
    if detect_summary_intent(text):
        return {
            "can_handle": True,
            "status": "ok",
            "grounded": True,
            "confidence": 0.9,
            "reason": "该问题明确要求总结、概括或复盘已有内容。",
        }
    return {
        "can_handle": False,
        "status": "unsupported",
        "grounded": False,
        "confidence": 0.0,
        "reason": "该问题不属于总结归纳技能范围。",
    }


def assess_date_query(text: str) -> Dict:
    value = (text or "").strip()
    if detect_date_intent(value):
        return {
            "can_handle": True,
            "status": "ok",
            "grounded": True,
            "confidence": 0.92,
            "reason": "该问题包含明确的日期/时间语义。",
        }
    return {
        "can_handle": False,
        "status": "unsupported",
        "grounded": False,
        "confidence": 0.0,
        "reason": "该问题不属于日期时间技能范围。",
    }


def assess_delegate_query(text: str) -> Dict:
    if detect_delegate_intent(text):
        return {
            "can_handle": True,
            "status": "ok",
            "grounded": False,
            "confidence": 0.82,
            "reason": "该问题需要方案制定、任务拆解或协调建议。",
        }
    return {
        "can_handle": False,
        "status": "unsupported",
        "grounded": False,
        "confidence": 0.0,
        "reason": "该问题不属于委托规划技能范围。",
    }
