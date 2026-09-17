#!/usr/bin/env python3
"""Normal-user v1 agent smoke for a real route and stateful virtual devices."""
from pathlib import Path
import json
import os
import socket
import subprocess
import sys
import tempfile
import time

PKG=Path(__file__).resolve().parents[1]
REPO=PKG.parents[2]
sys.path.insert(0,str(REPO/'benchmark'))
sys.path.insert(0,str(PKG/'harness'))
from run_eval import RpcClient,check_daemon
from conversation_client import send_turn,wait_turn_response
from business_sim.backend import BusinessServer
from business_sim.state import Store,scope_key

out=Path(tempfile.mkdtemp(prefix='agentflow-business-v1-connections-',dir='/tmp'))
for line in (PKG/'.env').read_text().splitlines():
    line=line.strip().removeprefix('export ')
    if line and not line.startswith('#') and '=' in line:
        key,_,value=line.partition('=')
        if key.strip().startswith(('AF_LLM_','OPENAI_IMAGE_','BUSINESS_','WEBSEARCH_')) and value.strip():
            os.environ[key.strip()]=value.strip().strip('"\'')
os.environ.update(BUSINESS_EXECUTION_MODE='real',BUSINESS_SIM_DB=str(out/'state.sqlite3'))
backend=BusinessServer(('127.0.0.1',0),out/'state.sqlite3').start()
with socket.socket() as sock:
    sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
env=os.environ.copy()
env.update(AF_COORDINATOR_VERSION='v1',AF_SESSIONLOG_ENABLED='1',AF_SESSIONLOG_DIR=str(out/'sessionlogs'),
           AF_TRACE_OUTPUT_DIR=str(out/'traces'),BUSINESS_BACKEND_URL=backend.url,
           PYTHON_PATH=str(Path(sys.executable).parent.parent),PATH=str(Path(sys.executable).parent)+os.pathsep+env.get('PATH',''),
           AF_SKIP_DOTENV='1')
config=json.loads((PKG/'daemon.json').read_text())
config['server'].update(listen=f'127.0.0.1:{port}',uds_biz_path=str(out/'biz.sock'),uds_ctrl_path=str(out/'ctrl.sock'))
config['daemon']['pid_file']=str(out/'daemon.pid')
config['trace']={'output_dir':str(out/'traces')}
(out/'daemon.json').write_text(json.dumps(config))
log=(out/'daemon.log').open('w')
proc=subprocess.Popen([str(REPO/'build/agentflowd'),'--config',str(out/'daemon.json'),'--package',str(PKG)],
                      cwd=REPO,env=env,stdout=log,stderr=subprocess.STDOUT)
print('V1_CONNECTION_SMOKE_DIR',out,flush=True)
results=[]
identity=('v1-normal-user','v1-normal-client','v1-normal-dialogue')
prompts=[
    '请帮我查一下从成都东站到成都市人民公园的驾车路线，给我距离和预计用时。不要把没有实时路况的数据说成避堵结果。',
    '请使用虚拟多设备环境做一次验收：把“晚间用药提醒”广播到测试手机和测试手表；再把测试手机上播放到125秒的《流浪地球》接力到测试电视，并告诉我设备回读结果。',
    '把刚才的测试手表设为离线，再向测试手机和测试手表广播“第二次提醒”，告诉我分别是否送达。',
]
try:
    for _ in range(100):
        if proc.poll() is not None: raise RuntimeError('daemon exited; see '+str(out/'daemon.log'))
        probe=RpcClient('127.0.0.1',port,timeout=5)
        try:
            check_daemon(probe,'business-simulator');break
        except (OSError,RuntimeError): time.sleep(.3)
        finally: probe.close()
    else: raise RuntimeError('daemon not ready')
    client=RpcClient('127.0.0.1',port,timeout=300);client.connect()
    for number,prompt in enumerate(prompts,1):
        started=time.monotonic()
        send_turn(client,'business-simulator',identity[0],identity[1],identity[2],prompt)
        response=wait_turn_response(client,time.time()+300)
        response.update(turn=number,user=prompt,seconds=round(time.monotonic()-started,2))
        results.append(response)
        print('TURN',number,'SECONDS',response['seconds'],'ERROR',response.get('error'),'TIMEOUT',response.get('timed_out'),flush=True)
        print('ANSWER',response.get('final_text',''),flush=True)
        (out/'responses.json').write_text(json.dumps(results,ensure_ascii=False,indent=2))
        if response.get('error') or response.get('timed_out'): break
    client.close()
    scope=scope_key(identity[0],identity[1])
    store=Store(out/'state.sqlite3')
    events=store.events(scope);state=store.read(scope)
    map_events=[e for e in events if e['skill']=='web-search' and e['result'].get('source')=='OpenStreetMap/OSRM']
    multi_events=[e for e in events if e['skill']=='multi-device']
    world=state.get('virtual_device_world',{})
    tasks=world.get('tasks',[]);world_events=world.get('events',[])
    second_broadcast=next((e for e in reversed(world_events) if e.get('type')=='broadcast' and e.get('content')=='第二次提醒'),None)
    receipts={r['device_id']:r['delivered'] for r in (second_broadcast or {}).get('receipts',[])}
    checks={
        'three_normal_turns':len(results)==3 and all(r.get('final_text') and not r.get('error') and not r.get('timed_out') for r in results),
        'v1_enabled':env['AF_COORDINATOR_VERSION']=='v1',
        'real_route_called':bool(map_events) and map_events[-1]['result']['UserOutputObject'].get('route_calculated') is True,
        'real_route_not_simulated':bool(map_events) and map_events[-1]['result'].get('simulated') is False,
        'virtual_actions_called':len(multi_events)>=2,
        'handoff_persisted':any(t.get('target_device_id')=='virtual-tv-1' and t.get('payload',{}).get('position_seconds')==125 for t in tasks),
        'offline_state_persisted':any(d['id']=='virtual-watch-1' and d['status']=='offline' for d in world.get('devices',[])),
        'per_device_delivery_observed':receipts.get('virtual-phone-1') is True and receipts.get('virtual-watch-1') is False,
        'simulation_label_preserved':bool(multi_events) and all(e['result'].get('simulated') is True and e['result'].get('physical_effect') is False for e in multi_events if e['result'].get('status')=='succeeded'),
    }
    evidence={'checks':checks,'responses':results,'events':events,'virtual_device_world':world}
    (out/'evidence.json').write_text(json.dumps(evidence,ensure_ascii=False,indent=2))
    print('CHECKS',json.dumps(checks,ensure_ascii=False),flush=True)
    raise SystemExit(0 if all(checks.values()) else 1)
finally:
    proc.terminate()
    try: proc.wait(timeout=12)
    except subprocess.TimeoutExpired: proc.kill();proc.wait()
    log.close();backend.close()
