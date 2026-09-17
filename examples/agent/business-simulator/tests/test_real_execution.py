"""Real local effects, external failure honesty, scheduling and HTTP boundaries."""
from datetime import datetime, timedelta, timezone
import json
import os
from pathlib import Path
import sys
import tempfile
from unittest.mock import patch
from urllib.request import Request, build_opener, ProxyHandler

import pytest

PACKAGE = Path(__file__).resolve().parents[1]
sys.path.insert(0,str(PACKAGE/'harness'))
from business_sim.services import dispatch
from business_sim.state import Store, scope_key
from business_sim import reminders
from business_sim.backend import BusinessServer
from business_sim.live_information import weather


@pytest.fixture
def real(tmp_path, monkeypatch):
    monkeypatch.setenv('BUSINESS_EXECUTION_MODE','real')
    monkeypatch.delenv('BUSINESS_BACKEND_URL',raising=False)
    store=Store(tmp_path/'state.sqlite3');scope=scope_key('real-test-user','real-test-chat')
    return store,scope


def call(real,skill,**args):
    store,scope=real
    return dispatch(skill,args,store=store,scope=scope)


def test_no_external_connection_never_mutates_fake_devices(real):
    store,scope=real;before=store.read(scope);store.reset(scope,before)
    calls=[('device-gui',{'action':'execute','app':'高德地图','operation':'navigate','destination':'市人民医院'}),
           ('movie-query',{'action':'play','title':'电影'}),('multi-device',{'action':'list','execution_target':'physical'})]
    for skill,args in calls:
        outcome=call(real,skill,**args)
        assert outcome['status']==('needs_input' if skill=='movie-query' else 'unsupported')
        assert outcome.get('execution','not_executed')!='executed'
        assert outcome['simulated'] is False
    assert store.read(scope)==before


def test_plan_preserves_route_and_recharge_constraints(real):
    outcome=call(real,'device-gui',action='plan',app='高德地图',operation='navigate',origin='当前位置',destination='市人民医院',avoid_congestion=True,route_preference='fastest')
    plan=outcome['UserOutputObject']['taskPlan']
    assert plan['execution']=='planned' and plan['route_calculated'] is False
    assert plan['steps'][1]=='定位当前位置'
    assert any('避免拥堵' in step for step in plan['steps'])
    assert any('最快' in step for step in plan['steps'])
    assert plan['link_kind']=='destination_search'
    outcome=call(real,'device-gui',action='plan',app='中国联通',operation='recharge',amount=88,phone_number='18610002895')
    assert '18610002895' in str(outcome) and '88' in str(outcome)


def test_document_is_a_real_versioned_file_and_cannot_escape(real):
    first=call(real,'office-work',action='save',key='../../outside',content='第一版正文')['UserOutputObject']['document']
    second=call(real,'office-work',action='save',key='../../outside',content='第二版正文')['UserOutputObject']['document']
    assert Path(first['artifact']['path']).read_text()=='第一版正文'
    assert Path(second['artifact']['path']).read_text()=='第二版正文'
    assert second['version']==2
    assert Path(second['artifact']['path']).is_relative_to(real[0].path.parent/'artifacts'/real[1])
    assert call(real,'office-work',action='query',key='../../outside')['simulated'] is False


def test_reminder_calendar_scheduler_retry_restart_and_cancel(real):
    store,scope=real
    args=dict(action='reminder_create',medicine='验收提醒',times=['08:00','20:00'],timezone='Asia/Shanghai',period='每天',request_id='reminder-once')
    created=call(real,'health-advice',**args)
    assert created['status']=='succeeded',created
    record=created['UserOutputObject']['medicationReminder']
    assert '未提供' in record['dosage']
    calendar=Path(record['calendar_file']['path']).read_text()
    assert calendar.count('BEGIN:VEVENT')==2
    assert calendar.count('BEGIN:VALARM')==2
    assert 'RRULE:FREQ=DAILY' in calendar
    assert call(real,'health-advice',**args)['replayed'] is True
    assert len(store.read(scope)['reminders'])==1
    # Controlled clock only in the test; the worker itself writes actual durable notifications.
    state=store.read(scope)
    item=state['reminders'][record['id']]
    item.update(created_at='2026-09-17T07:59:00+08:00',start_date='2026-09-17',end_date='2026-09-18')
    store.reset(scope,state)
    assert reminders.tick(store,datetime.fromisoformat('2026-09-17T08:01:00+08:00'))==1
    assert reminders.tick(Store(store.path),datetime.fromisoformat('2026-09-17T08:02:00+08:00'))==0
    assert reminders.tick(store,datetime.fromisoformat('2026-09-18T21:00:00+08:00'))==3
    assert len(reminders.notifications(store,scope))==4
    call(real,'health-advice',action='reminder_cancel',id=record['id'])
    assert reminders.tick(store,datetime.fromisoformat('2026-09-19T21:00:00+08:00'))==0


def test_expired_invalid_period_does_not_create_success(real):
    outcome=call(real,'health-advice',action='reminder_create',medicine='测试',times=['08:00'],start_date='2026-09-20',end_date='2026-09-19')
    assert outcome['status']=='failed'
    assert not real[0].read(real[1])['reminders']


def test_weather_computes_both_condition_outcomes_without_case_ids():
    def fixture(humidity):
        return {'hourly':{'time':['2026-09-17T08:00','2026-09-17T20:00'],'relative_humidity_2m':[humidity,humidity],
                         'temperature_2m':[20,18],'precipitation_probability':[10,20],'wind_speed_10m':[5,8]}}
    for humidity, expected in ((40,False),(90,True)):
        with patch('business_sim.live_information.fetch_json',return_value=[fixture(humidity)]*3):
            data=weather({'location':'川西','date':'本周'})['UserOutputObject']
        assert data['humidity_assessment']['is_humid'] is expected
        assert data['humidity_mean_pct']==humidity
        assert data['representative_points_only'] is True
        assert len(data['locations'])==3


def test_weather_failure_never_uses_fixture(real):
    with patch('business_sim.real_services.weather',side_effect=OSError('network unavailable')):
        outcome=call(real,'web-search',action='weather',location='北京')
    assert outcome['status']=='failed' and outcome['simulated'] is False
    assert outcome['execution_mode']=='real'


def test_http_state_identity_and_notifications(real):
    store,scope=real
    server=BusinessServer(('127.0.0.1',0),store.path).start()
    opener=build_opener(ProxyHandler({}))
    def request(path,payload=None,user='real-test-user'):
        req=Request(server.url+path,data=json.dumps(payload).encode() if payload else None,
                    headers={'X-Business-User':user,'X-Business-Client':'real-test-chat','Content-Type':'application/json'})
        with opener.open(req,timeout=5) as reply:return json.load(reply)
    try:
        outcome=request('/v1/services/user-memory',{'action':'write','key':'口味','value':'清淡'})
        assert outcome['status']=='succeeded' and outcome['transport']=='http'
        assert request('/v1/state')['preferences']['口味']=='清淡'
        assert request('/v1/state',user='other')['preferences']=={}
        assert request('/v1/notifications')['scheduler_running'] is True
        assert len(request('/v1/devices')['devices'])==16
    finally:
        server.close()


def test_weather_relative_weeks_and_days_are_not_silently_shifted():
    from urllib.parse import parse_qs,urlparse
    from business_sim import live_information
    class FixedClock(datetime):
        @classmethod
        def now(cls,tz=None):
            return datetime(2026,9,17,12,tzinfo=timezone.utc).astimezone(tz)
    payload={'hourly':{'time':['2026-09-17T08:00'],'relative_humidity_2m':[50],
                       'temperature_2m':[20],'precipitation_probability':[10],'wind_speed_10m':[5]}}
    for period,start,end in [('上周','2026-09-07','2026-09-13'),('下周一','2026-09-21','2026-09-21'),
                             ('下周','2026-09-21','2026-09-27'),('本周末','2026-09-19','2026-09-20')]:
        with patch.object(live_information,'datetime',FixedClock),patch.object(live_information,'fetch_json',return_value=payload) as fetch:
            answer=weather({'location':'北京','date':period})
            query=parse_qs(urlparse(fetch.call_args.args[0]).query)
        assert query['start_date']==[start] and query['end_date']==[end]
        assert answer['status']=='succeeded'


def test_calendar_lines_fold_utf8_without_losing_content(real):
    record=call(real,'health-advice',action='reminder_create',medicine='测'*80,times=['08:00'],period='每天')['UserOutputObject']['medicationReminder']
    raw=Path(record['calendar_file']['path']).read_bytes()
    assert all(len(line)<=75 for line in raw.split(b'\r\n'))
    assert ('SUMMARY:用药提醒：'+'测'*80) in raw.decode().replace('\r\n ','')
