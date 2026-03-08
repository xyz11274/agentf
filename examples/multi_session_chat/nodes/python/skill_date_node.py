#!/usr/bin/env python3
# Copyright (c) 2026 AgentFlow Authors
# Licensed under MIT License
#
# skill_date_node.py
# Python implementation of the date-calc node in skill-date-workflow.
#
# Inputs (AF_* environment variables):
#   AF_USER_TEXT   — user query text (e.g. "下周三几号")
#   AF_SESSION_ID  — session identifier
#   AF_OUTPUT_FORMAT — if "json", output a JSON object; else plain text
#
# Output:
#   When AF_OUTPUT_FORMAT=json:
#     {"date_result": "<answer>", "status": "ok"}
#   Otherwise:
#     Plain text answer on stdout
#
# This node replaces the skill_date.sh shell wrapper and directly invokes
# the date NLP parsing logic (previously in date_nlp_parser.py) plus a
# fallback "current date/time" response when no query matches.

import calendar
import datetime as dt
import json
import os
import re
import sys

# ---------------------------------------------------------------------------
# Date NLP parsing (inlined from date_nlp_parser.py for self-contained node)
# ---------------------------------------------------------------------------

WEEKDAY = {
    "周一": 0, "星期一": 0,
    "周二": 1, "星期二": 1,
    "周三": 2, "星期三": 2,
    "周四": 3, "星期四": 3,
    "周五": 4, "星期五": 4,
    "周六": 5, "星期六": 5,
    "周日": 6, "星期日": 6, "周天": 6, "星期天": 6,
}
CN_NUM = {"一": 1, "二": 2, "三": 3, "四": 4, "五": 5, "六": 6}


def find_weekday(text: str):
    for k, v in WEEKDAY.items():
        if k in text:
            return v, k
    return None, ""


def answer_for_relative_week(text: str):
    if ("周" not in text and "星期" not in text) or (
        "几号" not in text and "几月几号" not in text
    ):
        return None
    m = re.search(r"(本|下+)周", text)
    if not m:
        return None
    token = m.group(1)
    weeks_ahead = 0 if token == "本" else len(token)
    wd, wd_text = find_weekday(text)
    if wd is None:
        return None
    today = dt.date.today()
    monday_this_week = today - dt.timedelta(days=today.weekday())
    target = monday_this_week + dt.timedelta(days=7 * weeks_ahead + wd)
    prefix = "本周" if weeks_ahead == 0 else ("下" * weeks_ahead + "周")
    return {
        "matched": True,
        "date": target.isoformat(),
        "answer": (
            f"{prefix}{wd_text[-1]}是{target.month}月{target.day}日"
            f"（周{['一','二','三','四','五','六','日'][wd]}）。"
        ),
    }


def answer_for_month_nth_week(text: str):
    if "第" not in text or "周" not in text or (
        "几号" not in text and "几月几号" not in text
    ):
        return None
    wd, wd_text = find_weekday(text)
    if wd is None:
        return None
    m = re.search(r"(\d{1,2})月第([一二三四五六\d])周", text)
    if not m:
        return None
    month = int(m.group(1))
    nth_raw = m.group(2)
    nth = int(nth_raw) if nth_raw.isdigit() else CN_NUM.get(nth_raw)
    if not nth or month < 1 or month > 12:
        return None

    today = dt.date.today()
    year = today.year
    cal = calendar.Calendar(firstweekday=0)

    def resolve(y):
        weeks = cal.monthdayscalendar(y, month)
        c = 0
        for week in weeks:
            if week[wd] != 0:
                c += 1
                if c == nth:
                    return dt.date(y, month, week[wd])
        return None

    target = resolve(year)
    if target is None or target < today:
        target = resolve(year + 1)
    if target is None:
        return None
    return {
        "matched": True,
        "date": target.isoformat(),
        "answer": (
            f"{month}月第{nth}周{wd_text}是{target.month}月{target.day}日"
            f"（周{['一','二','三','四','五','六','日'][wd]}）。"
        ),
    }


def parse_date_query(text: str):
    """Try all parsers; return the first match or None."""
    for fn in (answer_for_month_nth_week, answer_for_relative_week):
        result = fn(text)
        if result:
            return result
    return None


# ---------------------------------------------------------------------------
# Fallback: current date / time info
# ---------------------------------------------------------------------------

def current_date_answer() -> str:
    now = dt.datetime.now(dt.timezone.utc)
    today = dt.date.today()
    weekday_names = ["一", "二", "三", "四", "五", "六", "日"]
    now_text = now.isoformat(timespec="seconds").replace("+00:00", "Z")
    return (
        f"当前UTC时间：{now_text}；"
        f"本地日期：{today.isoformat()}（周{weekday_names[today.weekday()]}）；"
        f"7天后：{(today + dt.timedelta(days=7)).isoformat()}。"
    )


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    user_text  = os.environ.get("AF_USER_TEXT", "").strip()
    json_mode  = os.environ.get("AF_OUTPUT_FORMAT", "") == "json"

    answer = ""
    if user_text:
        parsed = parse_date_query(user_text)
        if parsed and parsed.get("matched"):
            answer = parsed["answer"]

    if not answer:
        answer = current_date_answer()

    if json_mode:
        print(json.dumps({"date_result": answer, "status": "ok"}, ensure_ascii=False))
    else:
        print(answer)


if __name__ == "__main__":
    main()
