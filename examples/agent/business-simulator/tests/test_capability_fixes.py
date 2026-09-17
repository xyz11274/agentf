"""Business invariants from the gap audit, independent of model replies."""
from datetime import datetime, timedelta
from pathlib import Path
import sys
import pytest

PACKAGE=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(PACKAGE/'harness'))
from business_sim.services import dispatch
from business_sim.state import Store,scope_key
from business_sim import reminders


@pytest.fixture
def env(tmp_path,monkeypatch):
    monkeypatch.setenv('BUSINESS_EXECUTION_MODE','real')
    monkeypatch.delenv('BUSINESS_BACKEND_URL',raising=False)
    return Store(tmp_path/'state.sqlite3'),scope_key('owner','client-a')


def call(env,skill='health-advice',**args):
    return dispatch(skill,args,store=env[0],scope=env[1])


@pytest.mark.parametrize('hospital',['协和医院','朝阳医院','安贞医院','同仁医院','阜外医院','儿童医院','中医院','北医三院'])
def test_declared_test_hospitals_have_real_inventory(env,hospital):
    result=call(env,action='appointment_create',hospital=hospital,department='内科',date='明天下午3点')
    assert result['status']=='succeeded',result
    record=result['UserOutputObject']['appointment']
    assert record['time']=='15:00' and record['environment']=='business_test_system'
    assert record['fee']['source']=='test_catalog' and record['paid_amount']==0
    assert len(call(env,action='appointment_query',hospital=hospital)['UserOutputObject']['appointments'])==1


def test_evening_and_conflicting_times_never_book_morning(env):
    for args in ({'date':'明天晚上'},{'date':'明天下午3点','not_after':'14:00'}):
        result=call(env,action='appointment_create',department='内科',**args)
        assert result['status']!='succeeded'
    assert not env[0].read(env[1])['appointments']


def test_query_honors_time_patient_and_department_alias(env):
    for patient,period in [('甲','上午'),('乙','下午')]:
        result=call(env,action='appointment_create',hospital='阜外医院',department='心血管内科',patient=patient,date='明天'+period)
        assert result['status']=='succeeded'
    result=call(env,action='appointment_query',department='心血管内科',patient='乙',date='明天',time_period='下午')
    rows=result['UserOutputObject']['appointments']
    assert len(rows)==1 and rows[0]['patient']=='乙' and rows[0]['time']>='12:00'


def test_reminder_update_preserves_id_and_requires_meal_information(env):
    original=call(env,action='reminder_create',medicine='用户给定药名',times=['08:00','20:00'])['UserOutputObject']['medicationReminder']
    pending=call(env,action='reminder_update',id=original['id'],meal_offset_minutes=30)
    assert pending['status']=='needs_input'
    assert env[0].read(env[1])['reminders'][original['id']]['times']==['08:00','20:00']
    changed=call(env,action='reminder_update',id=original['id'],meal_offset_minutes=30,meal_times=['07:10','19:20'])['UserOutputObject']['medicationReminder']
    assert changed['id']==original['id'] and changed['times']==['07:40','19:50']
    assert changed['version']==2 and len(changed['versions'])==1
    assert len(env[0].read(env[1])['reminders'])==1


def test_weekday_schedule_and_calendar_agree_no_weekend_delivery(env):
    args=dict(action='reminder_create',medicine='测试药',times=['08:00'],period='周一到周五',start_date='2026-09-18',end_date='2026-09-21')
    record=call(env,**args)['UserOutputObject']['medicationReminder']
    assert 'BYDAY=MO,TU,WE,TH,FR' in Path(record['calendar_file']['path']).read_text()
    state=env[0].read(env[1]);r=state['reminders'][record['id']]
    r['created_at']='2026-09-17T22:00:00+08:00'
    env[0].reset(env[1],state)
    assert reminders.tick(env[0],datetime.fromisoformat('2026-09-21T21:00:00+08:00'))==2
    days=[n['scheduled_for'][:10] for n in reminders.notifications(*env)]
    assert days==['2026-09-18','2026-09-21']


def test_reminder_filter_and_retry_do_not_duplicate(env):
    a=call(env,action='reminder_create',medicine='甲',times=['08:00'])['UserOutputObject']['medicationReminder']
    again=call(env,action='reminder_create',medicine='甲',times=['08:00'])
    assert again['duplicate'] and again['UserOutputObject']['medicationReminder']['id']==a['id']
    b=call(env,action='reminder_create',medicine='乙',times=['20:00'])['UserOutputObject']['medicationReminder']
    call(env,action='reminder_cancel',id=b['id'])
    rows=call(env,action='reminder_query',id=a['id'],medicine='甲')['UserOutputObject']['reminders']
    assert [r['id'] for r in rows]==[a['id']]


def test_record_facts_versions_filters_and_delete(env):
    original=call(env,action='record_save',patient='甲',record_fields={'symptoms':'咳嗽三天','medication':'尚未服药'})['UserOutputObject']['healthRecord']
    assert original['record_fields']['medication']=='尚未服药'
    call(env,action='record_save',patient='乙',record_fields={'symptoms':'发热'})
    changed=call(env,action='record_save',id=original['id'],record_fields={'symptoms':'咳嗽好转'})['UserOutputObject']['healthRecord']
    assert changed['versions'][0]['record_fields']['symptoms']=='咳嗽三天'
    rows=call(env,action='record_query',patient='甲',id=original['id'])['UserOutputObject']['records']
    assert len(rows)==1 and rows[0]['version']==2
    call(env,action='record_delete',id=original['id'])
    assert call(env,action='record_query',patient='甲')['UserOutputObject']['records']==[]


def test_shared_user_assets_are_distinct_from_conversation_and_other_users(env):
    store,first=env;second=scope_key('owner','client-b');other=scope_key('different','client-a')
    call(env,'user-memory',action='write',key='偏好',value='科幻')
    call(env,'smart-home',action='control',commands=[{'device':'客厅灯','action':'on'}])
    assert call((store,second),'user-memory',action='read')['UserOutputObject']['preferences']['偏好']=='科幻'
    assert next(d for d in store.read(second)['devices'] if d['id']=='living-light')['power']
    assert not store.read(other)['preferences']
    call(env,'device-gui',action='plan',app='网站',operation='search',query='本会话内容')
    assert store.read(first)['gui']['plans'] and not store.read(second)['gui']['plans']


@pytest.mark.parametrize('op,extra,expected',[
    ('favorite',{'title':'目标作品'},'收藏'),('install',{'query':'目标应用'},'安装'),
    ('uninstall',{'query':'目标应用'},'卸载'),('checkin',{'page':'我的金币'},'签到'),
    ('comment',{'title':'目标作品','content':'指定评论'},'指定评论'),
    ('taxi',{'destination':'市人民医院','pickup_point':'东门','vehicle_type':'商务车'},'商务车'),
    ('send',{'recipient':'指定收件人','content':'指定正文'},'指定正文')])
def test_gui_plans_preserve_business_steps_and_parameters(env,op,extra,expected):
    result=call(env,'device-gui',action='plan',app='用户指定应用',operation=op,**extra)
    assert result['status']=='succeeded',result
    plan=result['UserOutputObject']['taskPlan']
    assert expected in '\n'.join(plan['steps'])
    assert all(plan['constraints'][k]==v for k,v in extra.items())
    assert result['execution']=='planned' and not plan['device_connected']
