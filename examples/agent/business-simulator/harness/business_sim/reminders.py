"""Durable daily reminders, calendar alarms and an idempotent local inbox worker."""
from __future__ import annotations

from datetime import datetime, timedelta, timezone, date
import json
import os
import re
import threading
import time
import uuid
from zoneinfo import ZoneInfo

from .live_information import result
from . import schedules


def tables(store):
    with store.connect() as db:
        db.executescript('''
            CREATE TABLE IF NOT EXISTS notifications (
                scope TEXT NOT NULL, reminder_id TEXT NOT NULL, scheduled_for TEXT NOT NULL,
                payload TEXT NOT NULL, created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
                PRIMARY KEY(scope, reminder_id, scheduled_for));
            CREATE TABLE IF NOT EXISTS worker_health (name TEXT PRIMARY KEY, heartbeat REAL NOT NULL);
        ''')


def running(store):
    tables(store)
    with store.connect() as db:
        row = db.execute("SELECT heartbeat FROM worker_health WHERE name='reminders'").fetchone()
    return bool(row and time.time()-row[0] < 30)


def escape(value):
    return str(value).replace('\\', '\\\\').replace('\n', '\\n').replace(';', '\\;').replace(',', '\\,').replace('\r', '')


def calendar(record):
    # UTC DTSTART avoids depending on a client's interpretation of an undefined TZID.
    lines = ['BEGIN:VCALENDAR', 'VERSION:2.0', 'PRODID:-//AgentFlow//Business Reminders//ZH', 'CALSCALE:GREGORIAN']
    zone = ZoneInfo(record['timezone'])
    for clock in record['times']:
        first_day = date.fromisoformat(record['start_date'])
        for _ in range(3660):
            if schedules.is_day(record, first_day):
                break
            first_day += timedelta(days=1)
        else:
            raise ValueError('所选周期没有可用的提醒日期')
        first = datetime.fromisoformat(first_day.isoformat()+'T'+clock).replace(tzinfo=zone)
        rule = schedules.rule(record)
        lines += ['BEGIN:VEVENT', f"UID:{record['id']}-{clock.replace(':','')}@agentflow.local",
                  'DTSTAMP:'+datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ'),
                  'DTSTART:'+first.astimezone(timezone.utc).strftime('%Y%m%dT%H%M%SZ'),
                  'DURATION:PT1M', 'SEQUENCE:'+str(record.get('version',1)), 'STATUS:'+('CONFIRMED' if record.get('active',True) else 'CANCELLED'), 'RRULE:'+rule, 'SUMMARY:'+escape('用药提醒：'+record['medicine']),
                  'DESCRIPTION:'+escape(f"用量：{record['dosage']}；周期：{record['period']}"),
                  'BEGIN:VALARM', 'TRIGGER:PT0S', 'ACTION:DISPLAY', 'DESCRIPTION:'+escape(record['medicine']),
                  'END:VALARM', 'END:VEVENT']
    # RFC5545 folds long content lines by octets, preserving UTF-8 characters.
    folded=[]
    for line in lines+['END:VCALENDAR', '']:
        fragment=''
        for character in line:
            if len((fragment+character).encode('utf-8'))>73:
                folded.append(fragment);fragment=' '+character
            else:
                fragment+=character
        folded.append(fragment)
    return '\r\n'.join(folded)


def create(state, args, store, scope):
    from .evaluation_world import clock
    if not args.get('medicine') or not (args.get('times') or args.get('meal_times')):
        return result('需要药名和提醒时刻；饭后提醒需要实际饭点。', status='needs_input', missing_fields=['medicine', 'times_or_meal_times'])
    zone = args.get('timezone') or os.environ.get('BUSINESS_TIMEZONE', 'Asia/Shanghai')
    now = clock(state, zone)
    try:
        schedule = schedules.normalize(args, now)
    except ValueError as exc:
        if str(exc) == 'end_date precedes start_date':
            raise
        return result(str(exc), status='needs_input', missing_fields=['schedule'])
    definition = {'medicine':args['medicine'], 'dosage':args.get('dosage') or '未提供；不推导剂量',
                  'patient':args.get('patient','当前用户'), 'timezone':zone, **schedule}
    old = state['reminders'].get(args.get('id')) if args['action']=='reminder_update' else None
    if args['action']=='reminder_update' and old is None:
        return result('提醒不存在。',status='not_found')
    if old is None and not args.get('allow_duplicate'):
        duplicate = next((r for r in state['reminders'].values() if r.get('active') and all(r.get(k)==v for k,v in definition.items())), None)
        if duplicate:
            return result('相同提醒已存在，未重复创建。', {'medicationReminder':duplicate}, operation_type='100151', duplicate=True)
    versions = list((old or {}).get('versions',[]))
    if old:
        versions.append({k:v for k,v in old.items() if k!='versions'})
    record = {**definition, 'id':old['id'] if old else 'reminder-'+uuid.uuid4().hex,
              'active':True, 'created_at':(old or {}).get('created_at',now.isoformat()),
              'updated_at':now.isoformat(), 'version':(old or {}).get('version',0)+1, 'versions':versions,
              'delivery':'server_inbox', 'phone_push_connected':False}
    if old:
        record['checked_through']=now.isoformat()  # Do not emit occurrences from before this edit.
    from .real_services import save_artifact
    record['calendar_file']=save_artifact(store,scope,record['id']+'.ics',calendar(record).encode('utf-8'),'text/calendar')
    state['reminders'][record['id']]=record
    return result('已更新原提醒。' if old else '已创建提醒及日历文件；到点写入本服务通知箱。',
                  {'medicationReminder':record}, operation_type='100151')


def manage(state, args, store, scope):
    from .evaluation_world import clock
    action=args['action']
    rows=list(state['reminders'].values())
    if action=='reminder_query':
        for field in ('id','medicine','patient'):
            if args.get(field):
                rows=[r for r in rows if r.get(field)==args[field]]
        if 'active' in args:
            rows=[r for r in rows if r.get('active')==args['active']]
        elif not args.get('include_inactive') and not args.get('id'):
            rows=[r for r in rows if r.get('active')]
        if args.get('date'):
            from .time_constraints import dates
            today=clock(state, os.environ.get('BUSINESS_TIMEZONE','Asia/Shanghai')).date()
            span=dates(args['date'],today)
            if not span or (span[1]-span[0]).days>366:
                return result('请明确一年内的查询日期范围。',status='needs_input',missing_fields=['date'])
            rows=[r for r in rows if any(schedules.is_day(r,span[0]+timedelta(days=i)) for i in range((span[1]-span[0]).days+1))]
        return result('已按条件查询提醒。',{'reminders':rows},operation_type='100157')
    item=state['reminders'].get(args.get('id'))
    if not item:
        return result('请指定已有提醒id。',status='not_found')
    if action=='reminder_update':
        merged={k:v for k,v in item.items() if k in ('medicine','dosage','patient','times','period','timezone','start_date','end_date')}
        merged.update(item.get('recurrence',{}))
        if 'period' in args:
            merged.pop('weekdays',None);merged.pop('interval_days',None)
        if 'times' in args and 'meal_offset_minutes' not in args:
            merged.pop('meal_times',None);merged.pop('meal_offset_minutes',None)
        return create(state,{**merged,**args},store,scope)
    now=clock(state, item['timezone']).isoformat()
    item['active']=action=='reminder_resume'
    item['status']='active' if item['active'] else 'paused' if action=='reminder_pause' else 'cancelled'
    item['version']=item.get('version',1)+1;item['updated_at']=now;item['checked_through']=now
    from .real_services import save_artifact
    item['calendar_file']=save_artifact(store,scope,item['id']+'.ics',calendar(item).encode(),'text/calendar')
    return result('已更新后台提醒状态；外部日历需重新同步。', {'reminders':[item]},operation_type='100157')


def notifications(store, scope):
    tables(store)
    scope = store.owner_scope(scope)
    with store.connect() as db:
        rows = db.execute('SELECT payload FROM notifications WHERE scope=? ORDER BY scheduled_for', (scope,)).fetchall()
    return [json.loads(row[0]) for row in rows]


def tick(store, now=None):
    """Catch up missed occurrences after restart; uniqueness survives retries/processes."""
    tables(store)
    now = now or datetime.now(timezone.utc)
    inserted = 0
    with store.connect() as db:
        db.execute('BEGIN IMMEDIATE')
        rows = db.execute('SELECT scope,payload FROM state').fetchall()
        for scope, payload in rows:
            state = json.loads(payload)
            changed = False
            for r in state.get('reminders', {}).values():
                if not r.get('active') or r.get('delivery') != 'server_inbox':
                    continue
                changed = True
                # 快照任务只随虚拟时钟推进，后台运行再久也不会产生额外通知。
                from .evaluation_world import clock
                local_now = clock(state, r['timezone']) if state.get('evaluation_world') else now.astimezone(ZoneInfo(r['timezone']))
                created = datetime.fromisoformat(r['created_at'])
                # Cursor advances with the same transaction as notification insertion.
                previous = datetime.fromisoformat(r.get('checked_through', r['created_at']))
                if local_now <= previous:
                    continue
                day = max(date.fromisoformat(r['start_date']), previous.date())
                end = min(date.fromisoformat(r['end_date']), local_now.date()) if r['end_date'] else local_now.date()
                while day <= end:
                    if not schedules.is_day(r,day):
                        day += timedelta(days=1)
                        continue
                    for clock in r['times']:
                        due = datetime.fromisoformat(day.isoformat()+'T'+clock).replace(tzinfo=local_now.tzinfo)
                        if max(created, previous) < due <= local_now:
                            item = {'id': r['id']+'@'+due.isoformat(), 'reminder_id': r['id'], 'medicine': r['medicine'],
                                    'dosage': r['dosage'], 'scheduled_for': due.isoformat(),
                                    'delivered_at': (local_now if state.get('evaluation_world') else now).isoformat(), 'channel': 'server_inbox', 'late': (local_now-due).total_seconds() > 60}
                            cursor = db.execute('INSERT OR IGNORE INTO notifications(scope,reminder_id,scheduled_for,payload) VALUES(?,?,?,?)',
                                                (scope, r['id'], due.isoformat(), json.dumps(item, ensure_ascii=False)))
                            inserted += cursor.rowcount
                    day += timedelta(days=1)
                r['checked_through'] = local_now.isoformat()
            if changed:
                db.execute('UPDATE state SET payload=? WHERE scope=?', (json.dumps(state, ensure_ascii=False), scope))
        db.execute("INSERT OR REPLACE INTO worker_health VALUES('reminders',?)", (time.time(),))
    return inserted


class ReminderWorker:
    def __init__(self, store):
        self.store = store
        self.stop_event = threading.Event()
        self.thread = threading.Thread(target=self.loop, name='business-reminders', daemon=True)

    def start(self):
        tick(self.store)
        self.thread.start()
        return self

    def loop(self):
        while not self.stop_event.wait(1):
            try:
                tick(self.store)
            except Exception:
                # Do not leak medical records or credentials into shared stderr.
                import logging
                logging.getLogger(__name__).exception('Reminder scheduler failed')

    def close(self):
        self.stop_event.set(); self.thread.join(timeout=5)
        with self.store.connect() as db:
            db.execute("DELETE FROM worker_health WHERE name='reminders'")
