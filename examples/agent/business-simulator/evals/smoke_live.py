#!/usr/bin/env python3
"""Four-turn development smoke test using the configured live LLM (consumes model quota).

Artifacts and a dedicated daemon live under /tmp; the daemon always stops on exit.
This is a state and routing check, not the full product dataset score.
"""
from pathlib import Path
import json,os,socket,subprocess,sys,tempfile,time
PKG=Path(__file__).resolve().parents[1];REPO=PKG.parents[2]
sys.path.insert(0,str(REPO/'benchmark'));sys.path.insert(0,str(PKG/'harness'))
from run_eval import RpcClient,check_daemon,send_turn,wait_turn_response
from business_sim.state import Store,scope_key
out=Path(tempfile.mkdtemp(prefix='agentflow-business-smoke-',dir='/tmp'))
env=os.environ.copy()
config_file=PKG/'.env'
for line in config_file.read_text().splitlines() if config_file.exists() else []:
 line=line.strip().removeprefix('export ')
 if line and not line.startswith('#') and '=' in line:
  k,_,v=line.partition('=')
  if k.strip() in ('AF_LLM_API_KEY','AF_LLM_BASE_URL','AF_LLM_MODEL','AF_LLM_THINKING_MODE') and v.strip():env[k.strip()]=v.strip().strip('"\'')
if not env.get('AF_LLM_API_KEY') or not env.get('AF_LLM_BASE_URL'):raise SystemExit('No usable model configuration for live smoke')
with socket.socket() as s:s.bind(('127.0.0.1',0));port=s.getsockname()[1]
env.setdefault('AF_LLM_MODEL','deepseek-v4-flash')
env.setdefault('AF_LLM_THINKING_MODE','disabled')
env.update(AF_COORDINATOR_VERSION='v1',AF_SESSIONLOG_ENABLED='1',AF_SESSIONLOG_DIR=str(out/'sessionlogs'),AF_TRACE_OUTPUT_DIR=str(out/'traces'),BUSINESS_SIM_DB=str(out/'state.sqlite3'),BUSINESS_EXECUTION_MODE='fixture',BUSINESS_SIM_INFORMATION_MODE='fixture',PYTHON_PATH=str(Path(sys.executable).parent.parent),PATH=str(Path(sys.executable).parent)+os.pathsep+env.get('PATH',''),AF_SKIP_DOTENV='1')
conf=json.loads((PKG/'daemon.json').read_text());conf['server'].update(listen=f'127.0.0.1:{port}',uds_biz_path=str(out/'biz.sock'),uds_ctrl_path=str(out/'ctrl.sock'));conf['daemon']['pid_file']=str(out/'daemon.pid');conf['trace']={'output_dir':str(out/'traces')}
(out/'daemon.json').write_text(json.dumps(conf));(out/'info.json').write_text(json.dumps({'port':port,'directory':str(out)}))
log=(out/'daemon.log').open('w');proc=subprocess.Popen([str(REPO/'build/agentflowd'),'--config',str(out/'daemon.json'),'--package',str(PKG)],cwd=REPO,env=env,stdout=log,stderr=subprocess.STDOUT)
print('SMOKE_DIR',out,flush=True)
results=[]
try:
 for _ in range(100):
  if proc.poll() is not None:raise RuntimeError('daemon exited during startup; see local log')
  probe=RpcClient('127.0.0.1',port,timeout=5)
  try:check_daemon(probe,'business-simulator');print('DAEMON_READY',flush=True);break
  except (OSError,RuntimeError):time.sleep(.3)
  finally:probe.close()
 else:raise RuntimeError('daemon not ready')
 client=RpcClient('127.0.0.1',port,timeout=240);client.connect()
 prompts=[
  '请把客厅的灯打开，同时帮我设个每天早晚8点吃维生素D的提醒。',
  '查一下客厅的灯现在开着吗？也查一下刚才设的吃药提醒。',
  '请记住我的电影偏好是动画，然后按这个偏好推荐三部适合全家看的电影。',
  '请播放刚才推荐的第二部。'
 ]
 for i,prompt in enumerate(prompts,1):
  print('TURN_START',i,flush=True);start=time.monotonic();send_turn(client,'business-simulator','business-smoke-user','business-smoke-chat','business-smoke-dialogue',prompt)
  result=wait_turn_response(client,time.time()+240);result.update(turn=i,user=prompt,seconds=round(time.monotonic()-start,2));results.append(result)
  print('TURN_DONE',i,'seconds',result['seconds'],'error',bool(result.get('error')),'timeout',result.get('timed_out'),'text',result.get('final_text','')[:300],flush=True)
  (out/'responses.json').write_text(json.dumps(results,ensure_ascii=False,indent=2))
  if result.get('error') or result.get('timed_out'):break
 client.close()
 store=Store(out/'state.sqlite3');state=store.read(scope_key('business-smoke-user','business-smoke-chat'));events=store.events(scope_key('business-smoke-user','business-smoke-chat'))
 checks={'four_turns':len(results)==4,'lamp_on':any(d['device']=='灯' and d['room']=='客厅' and d['power'] for d in state['devices']),
         'reminder_created':any(r['medicine']=='维生素D' and r['times']==['08:00','20:00'] for r in state['reminders'].values()),
         'memory_saved':'动画' in str(state['preferences'].get('电影偏好','')),'three_recommendations':len(state['last_movies'])==3,
         'second_movie_selected':bool(state['player']) and len(state['last_movies'])>=2 and state['player']['title']==state['last_movies'][1]['title'],
         'successful_responses':bool(results) and all(not r.get('error') and not r.get('timed_out') and r.get('final_text') for r in results)}
 session_events=[json.loads(line) for p in (out/'sessionlogs').rglob('*.jsonl') for line in p.read_text().splitlines() if line.strip()]
 skill_ends=[e.get('data',{}) for e in session_events if (e.get('type') or e.get('event'))=='skill.end']
 checks['skills_completed']=bool(skill_ends) and all(e.get('status')=='completed' for e in skill_ends)
 actions=[(e['skill'],e['action']) for e in events]
 checks['save_before_recommend']=('user-memory','write') in actions and ('movie-query','recommend') in actions and actions.index(('user-memory','write'))<actions.index(('movie-query','recommend'))
 checks['sessionlog_recorded']=bool(session_events)
 (out/'evidence.json').write_text(json.dumps({'checks':checks,'state':state,'events':events},ensure_ascii=False,indent=2));print('CHECKS',json.dumps(checks),flush=True)
 sys.exit(0 if all(checks.values()) else 1)
finally:
 proc.terminate()
 try:proc.wait(timeout=12)
 except subprocess.TimeoutExpired:proc.kill();proc.wait()
 log.close()
