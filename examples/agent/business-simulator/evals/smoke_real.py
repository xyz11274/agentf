#!/usr/bin/env python3
"""Real model + public weather + local task completion; no fixture business services.

Uses a temporary database/daemon, consumes model/image quota, and stops all workers.
Missing hardware is checked as missing, never counted as a completed action.
"""
from pathlib import Path
from datetime import datetime,timedelta,timezone
import json,os,socket,subprocess,sys,tempfile,time
PKG=Path(__file__).resolve().parents[1];REPO=PKG.parents[2]
sys.path.insert(0,str(REPO/'benchmark'));sys.path.insert(0,str(PKG/'harness'))
from run_eval import RpcClient,check_daemon
from conversation_client import send_turn,wait_turn_response
from business_sim.state import Store,scope_key
from business_sim.backend import BusinessServer
from business_sim.services import dispatch
from business_sim.reminders import notifications
out=Path(tempfile.mkdtemp(prefix='agentflow-business-real-',dir='/tmp'))
for line in (PKG/'.env').read_text().splitlines():
 line=line.strip().removeprefix('export ')
 if line and not line.startswith('#') and '=' in line:
  k,_,v=line.partition('=')
  if k.strip().startswith(('AF_LLM_','OPENAI_IMAGE_','BUSINESS_','WEBSEARCH_')) and v.strip():os.environ[k.strip()]=v.strip().strip('"\'')
os.environ.update(BUSINESS_EXECUTION_MODE='real',BUSINESS_SIM_DB=str(out/'state.sqlite3'))
backend=BusinessServer(('127.0.0.1',0),out/'state.sqlite3').start()
env=os.environ.copy()
with socket.socket() as s:s.bind(('127.0.0.1',0));port=s.getsockname()[1]
env.update(AF_COORDINATOR_VERSION='v1',AF_SESSIONLOG_ENABLED='1',AF_SESSIONLOG_DIR=str(out/'sessionlogs'),AF_TRACE_OUTPUT_DIR=str(out/'traces'),
           BUSINESS_BACKEND_URL=backend.url,PYTHON_PATH=str(Path(sys.executable).parent.parent),PATH=str(Path(sys.executable).parent)+os.pathsep+env.get('PATH',''),AF_SKIP_DOTENV='1')
conf=json.loads((PKG/'daemon.json').read_text());conf['server'].update(listen=f'127.0.0.1:{port}',uds_biz_path=str(out/'biz.sock'),uds_ctrl_path=str(out/'ctrl.sock'));conf['daemon']['pid_file']=str(out/'daemon.pid');conf['trace']={'output_dir':str(out/'traces')}
(out/'daemon.json').write_text(json.dumps(conf));log=(out/'daemon.log').open('w')
proc=subprocess.Popen([str(REPO/'build/agentflowd'),'--config',str(out/'daemon.json'),'--package',str(PKG)],cwd=REPO,env=env,stdout=log,stderr=subprocess.STDOUT)
print('REAL_SMOKE_DIR',out,flush=True)
case_only='--case-only' in sys.argv
results=[];scope=scope_key('real-smoke-user','real-smoke-chat');store=Store(out/'state.sqlite3')
try:
 # A separate actual wall-clock reminder must fire while the model runs.
 due=(datetime.now(timezone.utc)+timedelta(minutes=1)).replace(second=0,microsecond=0)
 timer_scope=scope_key('scheduler-probe','scheduler-probe')
 timer=dispatch('health-advice',{'action':'reminder_create','medicine':'服务调度验收事件','times':[due.strftime('%H:%M')],
                'timezone':'UTC','period':'每天','start_date':due.date().isoformat(),'end_date':due.date().isoformat()},store=store,scope=timer_scope)
 for _ in range(100):
  if proc.poll() is not None:raise RuntimeError('daemon exited; see log')
  probe=RpcClient('127.0.0.1',port,timeout=5)
  try:check_daemon(probe,'business-simulator');break
  except (OSError,RuntimeError):time.sleep(.3)
  finally:probe.close()
 else:raise RuntimeError('daemon not ready')
 case=next(c for c in json.loads((REPO/'benchmark/datasets/product.json').read_text())['cases'] if c['id']=='TGJK_YD2_010')
 prompts=[turn['user'] for turn in case['turns']]+[
  '另外帮我创建一个每天08:00和20:00的维生素D提醒，时区上海，剂量我还没提供。给我可导入日历的文件，并说明提醒实际发到哪里。',
  '请写一份三条事项的周末出行清单，保存成真实文件。另生成一张白底蓝色山峰的简洁插画，也给我实际图片文件。'
 ]
 if case_only:prompts=prompts[:2]
 client=RpcClient('127.0.0.1',port,timeout=300);client.connect()
 for i,prompt in enumerate(prompts,1):
  print('TURN_START',i,flush=True);start=time.monotonic()
  send_turn(client,'business-simulator','real-smoke-user','real-smoke-chat','real-smoke-dialogue',prompt)
  response=wait_turn_response(client,time.time()+300);response.update(turn=i,user=prompt,seconds=round(time.monotonic()-start,2));results.append(response)
  print('TURN_DONE',i,'seconds',response['seconds'],'error',bool(response.get('error')),'timeout',response.get('timed_out'),'text',response.get('final_text','')[:420],flush=True)
  (out/'responses.json').write_text(json.dumps(results,ensure_ascii=False,indent=2))
  (out/f'state-turn-{i}.json').write_text(json.dumps(store.read(scope),ensure_ascii=False,indent=2))
  if response.get('error') or response.get('timed_out'):break
 client.close()
 state=store.read(scope);events=store.events(scope)
 first=json.loads((out/'state-turn-1.json').read_text())
 weather_events=[e for e in events if e['skill']=='web-search' and e['result'].get('source')=='Open-Meteo']
 first_weather=weather_events[0]['result']['UserOutputObject'] if weather_events else {}
 humid=first_weather.get('humidity_assessment',{}).get('is_humid')
 reminders_created=list(state['reminders'].values())
 image_events=[e for e in events if e['skill']=='multimodal' and e['result'].get('UserOutputObject',{}).get('media_generated')]
 files=[f for e in events for f in e['result'].get('files',[])]
 docs=[d for bucket in state['documents'].values() for d in bucket.values()]
 checks={'four_turns':len(results)==4,'real_weather':bool(weather_events),
  'conditional_reminder':humid is not None and bool(first['reminders'])==(not humid),
  'navigation_plan':any(p['constraints'].get('avoid_congestion') and p['constraints'].get('route_preference')=='fastest' and p['execution']=='planned' for p in first['gui']['plans']),
  'explicit_reminder':any(r['times']==['08:00','20:00'] and '未提供' in r['dosage'] and Path(r['calendar_file']['path']).is_file() for r in reminders_created),
  'real_document':any(Path(d['artifact']['path']).is_file() for d in docs),
  'real_image':bool(image_events) and any(f['media_type'].startswith('image/') and Path(f['path']).stat().st_size>100 for f in files),
  'wall_clock_notification':bool(notifications(store,timer_scope)),
  'no_fixture_results':bool(events) and all(e['result'].get('simulated') is False for e in events),
  'successful_responses':len(results)==4 and all(not r.get('error') and not r.get('timed_out') and r.get('final_text') for r in results)}
 session_events=[json.loads(line) for p in (out/'sessionlogs').rglob('*.jsonl') for line in p.read_text().splitlines() if line.strip()]
 ends=[e.get('data',{}) for e in session_events if (e.get('type') or e.get('event'))=='skill.end']
 checks['followup_keeps_region']=len(results)>1 and '川西' in results[1].get('final_text','')
 checks['history_delivered']=any(e.get('type')=='llm.request' and len([m for m in e.get('data',{}).get('messages',[]) if m.get('role')=='user'])>=2 for e in session_events)
 checks['skills_completed']=bool(ends) and all(e.get('status')=='completed' for e in ends)
 if case_only:
  checks={k:v for k,v in checks.items() if k not in ('four_turns','explicit_reminder','real_document','real_image','successful_responses')}
  checks['two_turns']=len(results)==2 and all(not r.get('error') and not r.get('timed_out') for r in results)
 report={'checks':checks,'state':state,'events':events,'notifications':notifications(store,timer_scope),'notice':'Real development smoke, not full dataset accuracy. Navigation is planned; no physical device connected.'}
 (out/'evidence.json').write_text(json.dumps(report,ensure_ascii=False,indent=2));print('CHECKS',json.dumps(checks),flush=True)
 raise SystemExit(0 if all(checks.values()) else 1)
finally:
 proc.terminate()
 try:proc.wait(timeout=12)
 except subprocess.TimeoutExpired:proc.kill();proc.wait()
 log.close();backend.close()
