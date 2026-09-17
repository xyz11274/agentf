#!/usr/bin/env python3
"""Run fixed raw business cases against an isolated daemon, preserving scorer rules."""
from pathlib import Path
import argparse,json,os,socket,subprocess,sys,tempfile,time,hashlib
PKG=Path(__file__).resolve().parents[1];REPO=PKG.parents[2]
sys.path.insert(0,str(REPO/'benchmark'));sys.path.insert(0,str(PKG/'harness'))
from run_eval import RpcClient,check_daemon
from business_sim.backend import BusinessServer

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--label',required=True)
parser.add_argument('--cases',default='')
parser.add_argument('--judge',action='store_true')
parser.add_argument('--concurrency',type=int,default=1,choices=[1],help='串行使用每个daemon，避免会话池并发身份复用')
a=parser.parse_args()
for line in (PKG/'.env').read_text().splitlines():
 line=line.strip().removeprefix('export ')
 if line and not line.startswith('#') and '=' in line:
  k,_,v=line.partition('=')
  if k.strip().startswith(('AF_LLM_','OPENAI_IMAGE_','BUSINESS_','WEBSEARCH_')) and v.strip():os.environ[k.strip()]=v.strip().strip('"\'')
out=PKG/'outputs/representative'/a.label
out.mkdir(parents=True,exist_ok=True)
if (out/'report.json').exists():raise SystemExit('Refusing to replace existing report: '+str(out))
os.environ.update(BUSINESS_EXECUTION_MODE='real',BUSINESS_SIM_DB=str(out/'state.sqlite3'))
backend=BusinessServer(('127.0.0.1',0),out/'state.sqlite3').start()
with socket.socket() as sock:sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
env={**os.environ,'AF_SKIP_DOTENV':'1','AF_COORDINATOR_VERSION':'v1','AF_SESSIONLOG_ENABLED':'1',
     'AF_SESSIONLOG_DIR':str(out/'sessionlogs'),'AF_TRACE_OUTPUT_DIR':str(out/'traces'),
     'BUSINESS_BACKEND_URL':backend.url,'PYTHON_PATH':str(Path(sys.executable).parent.parent),
     'PATH':str(Path(sys.executable).parent)+os.pathsep+os.environ.get('PATH','')}
socket_dir=Path(tempfile.mkdtemp(prefix='af-biz-eval-'))
conf=json.loads((PKG/'daemon.json').read_text());conf['server'].update(listen=f'127.0.0.1:{port}',uds_biz_path=str(socket_dir/'biz.sock'),uds_ctrl_path=str(socket_dir/'ctrl.sock'));conf['daemon']['pid_file']=str(out/'daemon.pid');conf['trace']={'output_dir':str(out/'traces')}
(out/'daemon.json').write_text(json.dumps(conf))
files=sorted(p for p in PKG.rglob('*') if p.is_file() and p.suffix in ('.py','.md','.json') and not set(p.relative_to(PKG).parts) & {'outputs','.venv','__pycache__'})
files.append(REPO/'packages/agentflow-runtime/src/agentflow_runtime/coordinator_decide.py')
(out/'source-hashes.json').write_text(json.dumps({str(p.relative_to(REPO)):hashlib.sha256(p.read_bytes()).hexdigest() for p in files},indent=2))
log=(out/'daemon.log').open('w');proc=subprocess.Popen([str(REPO/'build/agentflowd'),'--config',str(out/'daemon.json'),'--package',str(PKG)],cwd=REPO,env=env,stdout=log,stderr=subprocess.STDOUT)
print('RUN_DIRECTORY',out,flush=True)
try:
 for _ in range(100):
  if proc.poll() is not None:raise RuntimeError('daemon exited; see log')
  c=RpcClient('127.0.0.1',port,timeout=3)
  try:check_daemon(c,'business-simulator');break
  except (OSError,RuntimeError):time.sleep(.2)
  finally:c.close()
 else:raise RuntimeError('daemon startup timeout')
 cases=a.cases or ','.join(json.loads((PKG/'evals/representative-30.json').read_text())['case_ids'])
 command=[sys.executable,str(PKG/'evals/run_product_eval.py'),'--port',str(port),'--cases',cases,'--concurrency',str(a.concurrency),'--turn-timeout','180',
          '--judge' if a.judge else '--no-judge','--output',str(out/'report.json')]
 child=subprocess.run(command,cwd=REPO,env=env)
 raise SystemExit(child.returncode)
finally:
 proc.terminate()
 try:proc.wait(timeout=12)
 except subprocess.TimeoutExpired:proc.kill();proc.wait()
 log.close();backend.close()
