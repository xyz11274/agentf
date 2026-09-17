"""Observe real persisted outputs through their public file/data contracts."""
import json, sys, zipfile
from pathlib import Path
from datetime import datetime,timezone
from unittest.mock import patch
import pytest
PKG=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(PKG/'harness'))
from business_sim.services import dispatch
from business_sim.state import Store,scope_key,fresh_state
from business_sim import device_world

@pytest.fixture
def env(tmp_path,monkeypatch):
    monkeypatch.setenv('BUSINESS_EXECUTION_MODE','real');monkeypatch.delenv('BUSINESS_BACKEND_URL',raising=False)
    return Store(tmp_path/'state.sqlite3'),scope_key('executing-owner','client')

def call(env,skill,**args):
    value=dispatch(skill,args,store=env[0],scope=env[1])
    return value

@pytest.mark.parametrize('fmt,xml',[('docx','word/document.xml'),('xlsx','xl/worksheets/sheet1.xml'),('pptx','ppt/slides/slide1.xml')])
def test_office_files_are_readable_zipped_documents(env,fmt,xml):
    args={'action':'save','title':'实际文件','content':'验收正文','format':fmt}
    if fmt=='xlsx':args['sheets']=[{'name':'预算','rows':[['项目','金额'],['交通',120],['=1+1',50]]}]
    result=call(env,'office-work',**args)
    assert result['status']=='succeeded',result
    path=Path(result['files'][0]['path'])
    with zipfile.ZipFile(path) as archive:
        assert archive.testzip() is None
        text=archive.read(xml).decode()
    if fmt=='xlsx':assert '<v>120</v>' in text and '<f>' not in text
    else:assert '验收正文' in text
    doc=result['UserOutputObject']['document']
    assert call(env,'office-work',action='query',key=doc['key'])['UserOutputObject']['documents'][0]['artifact']['sha256']==result['files'][0]['sha256']

def test_pdf_csv_and_calendar_are_actual_files(env):
    pdf=call(env,'office-work',action='save',title='PDF 文件',content='真实中文正文',format='pdf')
    assert pdf['status']=='succeeded',pdf
    raw=Path(pdf['files'][0]['path']).read_bytes()
    assert raw.startswith(b'%PDF-') and b'%%EOF' in raw
    csv=call(env,'office-work',action='save',format='csv',sheets=[{'rows':[['项目','金额'],['甲,乙',10]]}])
    assert '"甲,乙",10' in Path(csv['files'][0]['path']).read_text(encoding='utf-8-sig')
    calendar=call(env,'office-work',action='calendar_create',title='实际会议',start_time='2026-09-20T10:00:00+08:00',end_time='2026-09-20T11:00:00+08:00')
    text=Path(calendar['files'][0]['path']).read_text()
    assert 'DTSTART:20260920T020000Z' in text and 'SUMMARY:实际会议' in text
    assert calendar['UserOutputObject']['document']['invitation_sent'] is False

def test_sourced_movie_index_is_exact_and_filters_are_retained(env):
    items=[{'title':'甲','highlight':'描述甲','source_url':'https://example.org/a'},
           {'title':'乙','highlight':'描述乙','source_url':'https://example.org/b'}]
    assert call(env,'movie-query',action='save_recommendations',recommendations=items)['status']=='succeeded'
    detail=call(env,'movie-query',action='detail',index=2)
    assert detail['UserOutputObject']['movie']['title']=='乙'
    assert len(detail['UserOutputObject']['aiMovieData']['list'])==1
    assert call(env,'movie-query',action='detail',index=3)['status']=='not_found'
    assert call(env,'movie-query',action='save_recommendations',recommendations=items,exclude=['甲'])['status']=='failed'
    assert env[0].read(env[1])['last_movies']==items

def test_imported_knowledge_versions_are_owned_and_searchable(env):
    for text in ('我们提供试用，数据保留30天。','我们提供试用，数据保留45天。'):
        value=call(env,'knowledge-base',action='import_text',key='policy',title='用户提供的政策',content=text)
        assert value['status']=='succeeded'
    found=call(env,'knowledge-base',action='search_local',query='数据保留')
    row=found['UserOutputObject']['results'][0]
    assert row['version']==2 and '45天' in row['excerpt'] and row['source_kind']=='user_provided'
    assert call((env[0],scope_key('another-owner','client')),'knowledge-base',action='search_local',query='数据保留')['status']=='not_found'
    assert call((env[0],scope_key('executing-owner','second-client')),'knowledge-base',action='search_local',query='数据保留')['status']=='succeeded'
    call(env,'knowledge-base',action='delete_source',key='policy')
    assert call(env,'knowledge-base',action='search_local',query='数据保留')['status']=='not_found'

def test_workflow_executes_actual_tools_and_retry_does_not_repeat(env):
    steps=[{'skill':'user-memory','args':{'action':'write','key':'工作流值','value':'实际写入'}},
           {'skill':'office-work','args':{'action':'save','key':'workflow-document','content':'实际文件','format':'docx'}}]
    saved=call(env,'custom-workflow',action='workflow_save',steps=steps)['UserOutputObject']['workflow']
    args={'action':'workflow_run','key':saved['key'],'request_id':'one-run'}
    first=call(env,'custom-workflow',**args);again=call(env,'custom-workflow',**args)
    assert first['status']=='succeeded' and again['replayed']
    state=env[0].read(env[1])
    assert state['preferences']['工作流值']=='实际写入'
    assert state['documents']['office-work']['workflow-document']['version']==1
    assert Path(state['documents']['office-work']['workflow-document']['artifact']['path']).is_file()

def test_failed_workflow_preserves_receipts_and_does_not_execute_later_steps(env):
    steps=[{'skill':'user-memory','args':{'action':'write','key':'first','value':'yes'}},
           {'skill':'device-gui','args':{'action':'execute','app':'无设备','operation':'open'}},
           {'skill':'user-memory','args':{'action':'write','key':'third','value':'must not happen'}}]
    item=call(env,'custom-workflow',action='workflow_save',steps=steps)['UserOutputObject']['workflow']
    answer=call(env,'custom-workflow',action='workflow_run',key=item['key'],request_id='partial-run')
    assert answer['status']=='failed' and answer['execution']=='partially_executed'
    assert env[0].read(env[1])['preferences']=={'first':'yes'}
    assert answer['UserOutputObject']['completed_steps']==1 and len(answer['UserOutputObject']['trace'])==2

def test_home_energy_and_cooccurrence_are_integrated_by_day_and_device():
    state=fresh_state()
    start=datetime(2026,9,15,0,tzinfo=timezone.utc).timestamp()
    state['world']['last_tick']=start
    for d in state['devices']:d['power']=False
    device_world.execute(state,{'action':'control','commands':[{'device':'客厅灯','action':'on'},{'device':'客厅空调','action':'on'}]},now=start)
    device_world.advance(state,start+3600)
    answer=device_world.execute(state,{'action':'habits','analysis':'energy','date':'2026-09-15','device':'客厅空调'},now=start+3600)
    obj=answer['UserOutputObject']
    assert answer['operation_type']=='100053'
    assert obj['energy'][0]['energy_kwh']==pytest.approx(1.2)
    assert obj['energy'][0]['powered_hours']==1
    assert obj['cooccurrence'][0]['simultaneous_hours']==1
    assert obj['history'][0]['text']
    outside=device_world.execute(state,{'action':'habits','analysis':'energy','date':'2026-08-15'},now=start+3600)
    assert outside['UserOutputObject']['energy']==[]

def test_historical_weather_selects_archive_and_never_invents_rain_probability(env):
    data={'hourly':{'time':['2023-09-05T12:00'],'relative_humidity_2m':[63.],
                   'temperature_2m':[22.],'wind_speed_10m':[10.],'precipitation':[2.]}}
    with patch('business_sim.live_information.fetch_json',return_value=data) as fetch:
        answer=call(env,'web-search',action='weather',location='北京',date='2023-09-05')
    assert 'archive-api.open-meteo.com' in fetch.call_args.args[0]
    assert answer['status']=='succeeded'
    obj=answer['UserOutputObject'];day=obj['locations'][0]['daily'][0]
    assert 'reanalysis' in obj['data_kind'] and day['rain_probability_max_pct'] is None
    assert day['precipitation_sum_mm']==2

