"""Stateful virtual devices and configurable real-world connector boundaries."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import json
import sys
import threading
import socketserver
from unittest.mock import patch

import pytest

PKG=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(PKG/'harness'))
from business_sim.services import dispatch
from business_sim.state import Store,scope_key


@pytest.fixture
def env(tmp_path,monkeypatch):
    monkeypatch.setenv('BUSINESS_EXECUTION_MODE','real')
    monkeypatch.delenv('BUSINESS_BACKEND_URL',raising=False)
    return Store(tmp_path/'state.sqlite3'),scope_key('virtual-owner','client-a')


def call(env,skill,**args):
    return dispatch(skill,args,store=env[0],scope=env[1])


def test_virtual_fleet_is_stateful_and_clearly_labelled(env):
    listed=call(env,'multi-device',action='list')
    assert listed['status']=='succeeded' and listed['simulated'] is True
    assert listed['environment']=='business_test_system' and listed['physical_effect'] is False
    devices=listed['UserOutputObject']['devices']
    assert len(devices)==6 and {d['device_type'] for d in devices}>={'phone','desktop','tv','speaker','watch'}
    selected=call(env,'multi-device',action='select',device_id='virtual-phone-1',request_id='select-phone')
    assert selected['status']=='succeeded' and selected['simulated'] is True
    handoff=call(env,'multi-device',action='handoff',source_device_id='virtual-phone-1',
                 target_device_id='virtual-tv-1',task_kind='media',payload={'title':'测试影片','position_seconds':125},
                 request_id='handoff-one')
    assert handoff['status']=='succeeded' and handoff['execution']=='simulated_and_state_observed'
    state=env[0].read(env[1])['virtual_device_world']
    tv=next(d for d in state['devices'] if d['id']=='virtual-tv-1')
    assert tv['current_activity']['title']=='测试影片' and len(state['tasks'])==1
    replay=call(env,'multi-device',action='handoff',source_device_id='virtual-phone-1',
                target_device_id='virtual-tv-1',task_kind='media',payload={'title':'测试影片','position_seconds':125},
                request_id='handoff-one')
    assert replay['replayed'] is True and len(env[0].read(env[1])['virtual_device_world']['tasks'])==1


def test_virtual_offline_broadcast_and_user_shared_sync(env):
    call(env,'multi-device',action='set_online',device_id='virtual-watch-1',online=False,request_id='watch-off')
    sent=call(env,'multi-device',action='broadcast',content='该吃药了',request_id='broadcast-one')
    receipts={r['device_id']:r for r in sent['UserOutputObject']['receipts']}
    assert receipts['virtual-phone-1']['delivered'] is True
    assert receipts['virtual-watch-1']['delivered'] is False
    sync=call(env,'multi-device',action='sync',data_type='shopping-list',
              target_device_ids=['virtual-phone-1','virtual-desktop-1'],payload={'items':['牛奶','鸡蛋']},request_id='sync-one')
    assert sync['status']=='succeeded' and sync['UserOutputObject']['version']==1
    second=(env[0],scope_key('virtual-owner','client-b'))
    history=call(second,'multi-device',action='history')
    assert any(event['type']=='sync' for event in history['UserOutputObject']['events'])


def test_public_route_uses_geocoder_and_router_without_claiming_traffic(env):
    origin=[{'name':'起点','display_name':'成都市起点','lon':'104.06','lat':'30.67','type':'place','importance':1}]
    destination=[{'name':'终点','display_name':'成都市终点','lon':'104.10','lat':'30.70','type':'hospital','importance':1}]
    route={'code':'Ok','routes':[{'distance':6100.2,'duration':820.4,'legs':[{'steps':[{'distance':100,'duration':20,'name':'人民路','maneuver':{'type':'turn','modifier':'right'}}]}]}]}
    with patch('business_sim.map_services.fetch_json',side_effect=[origin,destination,route]):
        answer=call(env,'web-search',action='route',origin='成都东门',destination='成都市人民医院',travel_mode='driving')
    assert answer['status']=='succeeded' and answer['simulated'] is False
    obj=answer['UserOutputObject']
    assert obj['route_calculated'] is True and obj['traffic_data'] is False
    assert obj['selected_route']['duration_seconds']==820.4


class SMTPHandler(socketserver.StreamRequestHandler):
    def handle(self):
        self.wfile.write(b"220 test-smtp ESMTP\r\n")
        data=[];reading=False
        while True:
            line=self.rfile.readline()
            if not line:break
            if reading:
                if line==b".\r\n":
                    self.server.messages.append(b"".join(data));data=[];reading=False
                    self.wfile.write(b"250 queued as test-message\r\n")
                else:data.append(line)
                continue
            command=line.decode(errors="replace").upper()
            if command.startswith(("EHLO ","HELO ")):
                self.wfile.write(b"250-test-smtp\r\n250 SIZE 1048576\r\n")
            elif command.startswith(("MAIL FROM:","RCPT TO:")):
                self.wfile.write(b"250 ok\r\n")
            elif command.startswith("DATA"):
                reading=True;self.wfile.write(b"354 end with dot\r\n")
            elif command.startswith("QUIT"):
                self.wfile.write(b"221 bye\r\n");break
            else:self.wfile.write(b"250 ok\r\n")


def test_smtp_requires_confirmation_and_returns_server_receipt(env,monkeypatch):
    server=socketserver.ThreadingTCPServer(('127.0.0.1',0),SMTPHandler);server.messages=[]
    thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
    monkeypatch.setenv('BUSINESS_SMTP_HOST','127.0.0.1')
    monkeypatch.setenv('BUSINESS_SMTP_PORT',str(server.server_address[1]))
    monkeypatch.setenv('BUSINESS_SMTP_SECURITY','none')
    monkeypatch.setenv('BUSINESS_SMTP_FROM','agent@example.test')
    preview=call(env,'office-work',action='email_send',to='user@example.test',subject='测试',content='正文')
    assert preview['status']=='needs_confirmation'
    try:
        sent=call(env,'office-work',action='email_send',to='user@example.test',subject='测试',content='正文',
                  confirmed=True,request_id='mail-one')
    finally:
        server.shutdown();server.server_close();thread.join()
    assert sent['status']=='succeeded' and sent['execution']=='accepted_by_server'
    assert sent['simulated'] is False and len(server.messages)==1
    assert b'user@example.test' in server.messages[0]


class Hook(BaseHTTPRequestHandler):
    received=[]
    def log_message(self,*args): pass
    def do_POST(self):
        raw=self.rfile.read(int(self.headers['Content-Length']))
        type(self).received.append((json.loads(raw),self.headers.get('Idempotency-Key')))
        body=b'{"accepted":true}'
        self.send_response(202);self.send_header('Content-Length',str(len(body)));self.end_headers();self.wfile.write(body)


def test_allowlisted_webhook_posts_real_http_request(env,monkeypatch):
    server=ThreadingHTTPServer(('127.0.0.1',0),Hook)
    thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
    origin=f'http://127.0.0.1:{server.server_port}'
    monkeypatch.setenv('BUSINESS_WEBHOOK_ALLOWED_ORIGINS',origin)
    monkeypatch.setenv('BUSINESS_WEBHOOK_ALLOW_HTTP','1')
    monkeypatch.setenv('BUSINESS_WEBHOOK_ALLOW_PRIVATE','1')
    try:
        answer=call(env,'third-party',action='webhook_post',url=origin+'/events',payload={'event':'done'},
                    confirmed=True,request_id='hook-one')
    finally:
        server.shutdown();server.server_close();thread.join()
    assert answer['status']=='succeeded' and answer['UserOutputObject']['http_status']==202
    assert answer['simulated'] is False and Hook.received[-1]==({'event':'done'},'hook-one')
