"""Validated reminder recurrence shared by the worker and exported calendars."""
from datetime import datetime, timedelta, date
import math
import re
from zoneinfo import ZoneInfo
from .time_constraints import number

DAYS = ["MO", "TU", "WE", "TH", "FR", "SA", "SU"]


def normalize(args, now):
    start = date.fromisoformat(args.get('start_date') or now.date().isoformat())
    period = args.get('period') or '每天，直到取消'
    end = args.get('end_date') or ''
    recurrence = {'kind':'daily', 'interval_days':1, 'weekdays':list(range(7))}
    weekdays = args.get('weekdays')
    duration = re.fullmatch(r'(?:连续|每天)?([一二三四五六七八九十两\d]+)天', period)
    interval = re.fullmatch(r'每(?:隔)?([一二三四五六七八九十两\d]+)天', period)
    if weekdays is not None:
        if not weekdays or any(isinstance(n,bool) or not isinstance(n,int) or not 0<=n<=6 for n in weekdays):
            raise ValueError('weekdays 需要周一=0 至周日=6 的非空数组')
        recurrence.update(kind='weekly',weekdays=sorted(set(weekdays)))
    elif period in ('工作日','每个工作日','周一到周五','每周一到周五'):
        recurrence.update(kind='weekly',weekdays=[0,1,2,3,4])
    elif period in ('周末','每周末'):
        recurrence.update(kind='weekly',weekdays=[5,6])
    elif period.startswith('每周'):
        tail=period[2:].replace('星期','').replace('周','')
        if not re.fullmatch(r'[一二三四五六日天、,，和及\s]+',tail):
            raise ValueError('请明确每周的日期')
        recurrence.update(kind='weekly',weekdays=sorted({'一二三四五六日'.index(c.replace('天','日')) for c in tail if c in '一二三四五六日天'}))
    elif period == '隔天' or interval:
        value=2 if period=='隔天' else number(interval[1])
        if not value or value>365:
            raise ValueError('天数间隔应为1至365')
        recurrence.update(interval_days=value)
    elif period in ('一周','两周','一个月','一月'):
        if not end:
            raise ValueError('请明确用药截止日期')
    elif duration:
        count=number(duration[1])
        if not count or count>3650:
            raise ValueError('用药周期天数无效')
        end=end or (start+timedelta(days=count-1)).isoformat()
    elif period not in ('每天','每日','长期','每天，直到取消','未指定'):
        # Supplying end_date must never bypass recurrence validation.
        raise ValueError('无法识别重复周期，请给出每日、每周日期或间隔天数')
    if args.get('interval_days'):
        recurrence['interval_days']=args['interval_days']
    if end and date.fromisoformat(end)<start:
        raise ValueError('end_date precedes start_date')
    times=args.get('times',[])
    if args.get('meal_offset_minutes') is not None:
        meals=args.get('meal_times')
        if not meals:
            raise ValueError('饭后提醒需要实际饭点 meal_times，未修改原提醒')
        times=[]
        for value in meals:
            if not re.fullmatch(r'(?:[01]\d|2[0-3]):[0-5]\d',value):
                raise ValueError('meal_times 必须为 HH:MM')
            h,m=map(int,value.split(':'));offset=h*60+m+args['meal_offset_minutes']
            if not 0<=offset<1440:
                raise ValueError('跨日的饭后提醒请明确次日时刻')
            times.append(f'{offset//60:02d}:{offset%60:02d}')
        recurrence.update(meal_times=meals,meal_offset_minutes=args['meal_offset_minutes'])
    if not times or any(not re.fullmatch(r'(?:[01]\d|2[0-3]):[0-5]\d',t) for t in times):
        raise ValueError('times 必须包含有效 HH:MM 时刻')
    return {'times':sorted(set(times)), 'start_date':start.isoformat(), 'end_date':end,
            'period':period, 'recurrence':recurrence}


def is_day(record, day):
    rec=record.get('recurrence',{})
    start=date.fromisoformat(record['start_date'])
    end=date.fromisoformat(record['end_date']) if record.get('end_date') else None
    return (day>=start and (end is None or day<=end) and day.weekday() in rec.get('weekdays',list(range(7)))
            and (day-start).days % rec.get('interval_days',1)==0)


def rule(record):
    r=record.get('recurrence',{})
    interval=r.get('interval_days',1)
    parts=['FREQ=DAILY',f'INTERVAL={interval}']
    if r.get('weekdays') is not None and len(r['weekdays'])<7:
        parts.append('BYDAY='+','.join(DAYS[d] for d in r['weekdays']))
    if record.get('end_date'):
        last=datetime.fromisoformat(record['end_date']+'T23:59:59').replace(tzinfo=ZoneInfo(record['timezone']))
        from datetime import timezone
        parts.append('UNTIL='+last.astimezone(timezone.utc).strftime('%Y%m%dT%H%M%SZ'))
    return ';'.join(parts)
