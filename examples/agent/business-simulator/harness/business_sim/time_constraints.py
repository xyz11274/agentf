"""Conservative date/clock parsing shared by local business services."""
from datetime import datetime, timedelta
import re


def number(text):
    if text.isdigit():
        return int(text)
    digits = {c: i for i, c in enumerate("零一二三四五六七八九")}
    digits["两"] = 2
    if "十" in text:
        tens, _, units = text.partition("十")
        return (digits.get(tens, 1) * 10) + digits.get(units, 0)
    return digits.get(text)


def dates(text, today, *, default_days=None):
    if not text:
        return (today, today + timedelta(days=default_days-1)) if default_days else None
    explicit = re.findall(r"\d{4}-\d{2}-\d{2}", text)
    if explicit:
        parsed = [datetime.strptime(d, "%Y-%m-%d").date() for d in explicit]
        if len(parsed) > 2 or parsed[-1] < parsed[0]:
            raise ValueError("日期范围无效")
        return parsed[0], parsed[-1]
    month_day = re.search(r"(?:(\d{4})年)?(\d{1,2})月(\d{1,2})[日号]", text)
    if month_day:
        y, m, d = month_day.groups()
        day = today.replace(year=int(y or today.year), month=int(m), day=int(d))
        return day, day
    for word, offset in (("大后天", 3), ("后天", 2), ("明天", 1), ("今天", 0), ("昨天", -1), ("前天", -2)):
        if word in text:
            day = today + timedelta(days=offset)
            return day, day
    week = today-timedelta(days=today.weekday())
    shift = 14 if "下下周" in text else 7 if "下周" in text else -7 if "上周" in text else 0
    if "未来一周" in text or "未来7天" in text:
        return today, today+timedelta(days=6)
    if "周末" in text:
        start = week + timedelta(days=shift+5)
        return start, start+timedelta(days=1)
    weekday = re.search(r"(?:周|星期)([一二三四五六日天])", text)
    if weekday:
        day = week+timedelta(days=shift+"一二三四五六日".index(weekday[1].replace("天", "日")))
        if day < today and not any(w in text for w in ("上周", "本周", "这周", "下周")):
            day += timedelta(days=7)
        return day, day
    if any(w in text for w in ("上周", "下周", "本周", "这周")):
        return week+timedelta(days=shift), week+timedelta(days=shift+6)
    if "上个月" in text or "上月" in text:
        end = today.replace(day=1)-timedelta(days=1)
        return end.replace(day=1), end
    if "本月" in text or "这个月" in text:
        return today.replace(day=1), today
    return None


def clock_constraints(args):
    """Return inclusive lower/upper clock bounds and an optional exact clock.

    Unknown explicit periods are errors, never ignored. Chinese hour numbers
    are interpreted only with unambiguous AM/PM context (or 24-hour numbers).
    """
    text = " ".join(str(args.get(k, "")) for k in ("date", "time_period"))
    periods = [(0, 359, ("凌晨",)), (360, 719, ("上午", "早上", "早晨", "morning")),
               (720, 839, ("中午",)), (720, 1079, ("下午", "afternoon")),
               (1080, 1439, ("晚上", "晚间", "夜间", "evening", "night"))]
    bounds = [(lo, hi) for lo, hi, words in periods if any(w in text for w in words)]
    if len(set(bounds)) > 1:
        raise ValueError("请给出一个明确的就诊时段")
    if args.get("time_period") and not bounds and args["time_period"] not in ("全天", "all_day"):
        raise ValueError("不支持的时段，请指定上午、下午、晚上或全天")
    lo, hi = bounds[0] if bounds else (0, 1439)
    for field, lower in (("not_before", True), ("not_after", False)):
        if args.get(field):
            if not re.fullmatch(r"(?:[01]\d|2[0-3]):[0-5]\d", args[field]):
                raise ValueError(field+" 必须为 HH:MM")
            h, m = map(int, args[field].split(":"))
            if lower:
                lo = max(lo, h*60+m)
            else:
                hi = min(hi, h*60+m)
    exact = None
    match = re.search(r"(?<!\d)(\d{1,2})[:：]([0-5]\d)", text)
    chinese = re.search(r"([零一二三四五六七八九十两\d]{1,3})[点时](半|一刻|三刻|[零一二三四五六七八九十两\d]{1,3}分?)?", text)
    if match or chinese:
        h = int(match[1]) if match else number(chinese[1])
        minute = match[2] if match else chinese[2] or "零"
        m = int(minute) if match else {"半": 30, "一刻": 15, "三刻": 45}.get(minute, number(minute.removesuffix("分")))
        if h is None or m is None or h > 23 or m > 59:
            raise ValueError("时刻无效")
        if bounds and lo >= 720 and h < 12:
            h += 12
        elif not bounds and h < 12 and not match:
            raise ValueError("请说明上午或下午，或使用24小时制时刻")
        exact = h*60+m
        if not lo <= exact <= hi:
            raise ValueError("指定时刻与时段限制冲突")
    if not args.get("not_before") and any(w in text for w in ("别太早", "不要太早", "晚一点")):
        lo = max(lo, 900 if lo >= 720 else 600)
    if lo > hi:
        raise ValueError("时刻范围无效")
    return lo, hi, exact


def matches_clock(clock, constraint):
    lo, hi, exact = constraint
    h, m = map(int, clock.split(":"))
    value = h*60+m
    return lo <= value <= hi and (exact is None or value == exact)
