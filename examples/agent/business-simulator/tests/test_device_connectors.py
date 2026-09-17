"""Protocol-level tests for optional physical device adapters."""
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
from pathlib import Path
import json,os,stat,sys,threading
import pytest
PKG=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(PKG/'harness'))
from business_sim.services import dispatch
from business_sim.state import Store,scope_key

@pytest.fixture
def env(tmp_path,monkeypatch):
    monkeypatch.setenv('BUSINESS_EXECUTION_MODE','real');monkeypatch.delenv('BUSINESS_BACKEND_URL',raising=False)
    return Store(tmp_path/'state.sqlite3'),scope_key('connector-user','client')

class HA(BaseHTTPRequestHandler):
    state='off';temperature=26
    def log_message(self,*args):pass
    def reply(self,value):
        raw=json.dumps(value).encode();self.send_response(200);self.send_header('Content-Type','application/json');self.send_header('Content-Length',str(len(raw)));self.end_headers();self.wfile.write(raw)
    def authorized(self):return self.headers.get('Authorization')=='Bearer test-token'
    def do_GET(self):
        if not self.authorized():self.send_error(401);return
        self.reply([{'entity_id':'climate.living_room','state':self.state,'attributes':{'friendly_name':'客厅空调','temperature':self.temperature}}])
    def do_POST(self):
        if not self.authorized():self.send_error(401);return
        payload=json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        if self.path.endswith('/turn_on'):type(self).state='on'
        if self.path.endswith('/set_temperature'):type(self).temperature=payload['temperature']
        self.reply([])

@pytest.fixture
def ha(monkeypatch):
    server=ThreadingHTTPServer(('127.0.0.1',0),HA);thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
    monkeypatch.setenv('HOME_ASSISTANT_URL',f'http://127.0.0.1:{server.server_port}')
    monkeypatch.setenv('HOME_ASSISTANT_TOKEN','test-token');monkeypatch.setenv('HOME_ASSISTANT_ALLOW_HTTP','1')
    yield server
    server.shutdown();server.server_close();thread.join()

def call(env,skill,**args):
    return dispatch(skill,args,store=env[0],scope=env[1])

def test_home_assistant_query_control_and_replay(env,ha):
    query=call(env,'smart-home',action='query',execution_target='home_assistant',device='客厅空调')
    assert query['status']=='succeeded' and query['source']=='home-assistant'
    entity=query['UserOutputObject']['devices'][0]['entity_id']
    missing=call(env,'smart-home',action='control',execution_target='home_assistant',
                 commands=[{'device':'空调','device_id':entity,'action':'on'}])
    assert missing['status']=='needs_input'
    args={'action':'control','execution_target':'home_assistant','request_id':'physical-one',
          'commands':[{'device':'空调','device_id':entity,'action':'on'},
                      {'device':'空调','device_id':entity,'action':'set','property':'temperature','value':24}]}
    done=call(env,'smart-home',**args);again=call(env,'smart-home',**args)
    assert done['status']=='succeeded' and done['environment']=='physical_home'
    assert done['physical_effect']=='state_observed_not_physical_sensor_verified'
    assert again['replayed'] and HA.state=='on' and HA.temperature==24

def fake_adb(tmp_path,monkeypatch):
    script=tmp_path/'adb'
    script.write_text('''#!'''+sys.executable+'''
import base64,os,sys
args=sys.argv[1:]
if args==['devices','-l']:
 print('List of devices attached');print('serial-1 device product:test model:Pixel_Test')
elif 'uiautomator' in args:
 print('<?xml version="1.0"?><hierarchy><node text="搜索" content-desc="" resource-id="app:id/search" class="android.widget.EditText" clickable="true" password="false" package="test.app" bounds="[10,20][210,100]"/></hierarchy>')
elif 'screencap' in args:
 sys.stdout.buffer.write(base64.b64decode('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII='))
else:
 with open(os.environ['ADB_TEST_LOG'],'a') as f:f.write(' '.join(args)+'\\n')
''')
    script.chmod(script.stat().st_mode|stat.S_IXUSR)
    log=tmp_path/'adb.log';monkeypatch.setenv('BUSINESS_ADB_EXECUTABLE',str(script));monkeypatch.setenv('ADB_TEST_LOG',str(log))
    return log

def test_adb_list_select_observe_and_tap(env,tmp_path,monkeypatch):
    log=fake_adb(tmp_path,monkeypatch)
    listed=call(env,'multi-device',action='list',execution_target='physical')
    assert listed['status']=='succeeded' and listed['UserOutputObject']['devices'][0]['id']=='serial-1'
    assert call(env,'multi-device',action='select',device_id='serial-1')['status']=='succeeded'
    observed=call(env,'device-gui',action='device_observe',device_id='serial-1')
    element=observed['UserOutputObject']['elements'][0]
    assert element['text']=='搜索' and element['editable']
    assert Path(observed['files'][0]['path']).is_file()
    missing=call(env,'device-gui',action='device_tap',device_id='serial-1',ref=element['ref'])
    assert missing['status']=='needs_input'
    tapped=call(env,'device-gui',action='device_tap',device_id='serial-1',ref=element['ref'],request_id='tap-once')
    replay=call(env,'device-gui',action='device_tap',device_id='serial-1',ref=element['ref'],request_id='tap-once')
    assert tapped['status']=='succeeded' and replay['replayed']
    assert log.read_text().count('input tap 110 60')==1

def test_configured_but_unreachable_home_assistant_is_not_connected(env,monkeypatch):
    monkeypatch.setenv('HOME_ASSISTANT_URL','http://127.0.0.1:1')
    monkeypatch.setenv('HOME_ASSISTANT_TOKEN','configured-but-unreachable')
    monkeypatch.delenv('HOME_ASSISTANT_ALLOW_HTTP',raising=False)
    answer=call(env,'multi-device',action='list',execution_target='physical')
    assert answer['status']=='unsupported'
    assert all(d['id']!='home-assistant' for d in answer['UserOutputObject']['devices'])


def test_invalid_adb_executable_is_reported_as_no_device(env,monkeypatch):
    monkeypatch.setenv('BUSINESS_ADB_EXECUTABLE','/does/not/exist/adb')
    answer=call(env,'multi-device',action='list',execution_target='physical')
    assert answer['status']=='unsupported'
    assert answer['UserOutputObject']['devices']==[]


def test_no_config_never_claims_a_physical_device(env,monkeypatch):
    for key in ('HOME_ASSISTANT_URL','HOME_ASSISTANT_TOKEN','BUSINESS_ADB_EXECUTABLE'):
        monkeypatch.delenv(key,raising=False)
    assert call(env,'smart-home',action='query',execution_target='home_assistant')['status']=='unsupported'
    answer=call(env,'multi-device',action='list',execution_target='physical')
    assert answer['status']=='unsupported' and answer['UserOutputObject']['devices']==[]

