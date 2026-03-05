#!/usr/bin/env python3
import calendar
import datetime as dt
import json
import re
import sys

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
    if ("周" not in text and "星期" not in text) or ("几号" not in text and "几月几号" not in text):
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
        "kind": "relative_week",
        "date": target.isoformat(),
        "answer": f"{prefix}{wd_text[-1]}是{target.month}月{target.day}日（周{['一','二','三','四','五','六','日'][wd]}）。",
    }


def answer_for_month_nth_week(text: str):
    if "第" not in text or "周" not in text or ("几号" not in text and "几月几号" not in text):
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
        "kind": "month_nth_week",
        "date": target.isoformat(),
        "answer": f"{month}月第{nth}周{wd_text}是{target.month}月{target.day}日（周{['一','二','三','四','五','六','日'][wd]}）。",
    }


def main():
    q = sys.stdin.read().strip()
    if not q:
        print(json.dumps({"matched": False, "answer": "", "reason": "empty"}, ensure_ascii=False))
        return

    for fn in (answer_for_month_nth_week, answer_for_relative_week):
        r = fn(q)
        if r:
            print(json.dumps(r, ensure_ascii=False))
            return

    print(json.dumps({"matched": False, "answer": "", "reason": "no_pattern"}, ensure_ascii=False))


if __name__ == "__main__":
    main()
