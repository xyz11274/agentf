#!/usr/bin/env python3
"""Actual coordinator + skill models + Chromium, with three concurrent user identities.

The local website is a declared integration fixture. This does not establish
commercial shopping, mail sending, hospital or native phone acceptance.
"""
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime,timezone
from http.server import ThreadingHTTPServer
from pathlib import Path
import hashlib,json,os,socket,subprocess,sys,tempfile,threading,time
PKG=Path(__file__).resolve().parents[1];REPO=PKG.parents[2]
sys.path[:0]=[str(REPO/'benchmark'),str(PKG/'harness'),str(PKG/'tests'),str(PKG/'evals')]
from run_eval import RpcClient,check_daemon
from conversation_client import send_turn,wait_turn_response
from business_sim.backend import BusinessServer
from business_sim.state import scope_key
from test_browser_execution import Site

def main():
    for line in (PKG/'.env').read_text().splitlines():
        line=line.strip().removeprefix('export ')
        if line and not line.startswith('#') and '=' in line:
            key,_,value=line.partition('=')
            if key.strip().startswith(('AF_LLM_','OPENAI_IMAGE_','BUSINESS_','WEBSEARCH_')) and value.strip():
                os.environ[key.strip()]=value.strip().strip('"\'')
    out=PKG/'outputs/real-world'/datetime.now(timezone.utc).strftime('model-%Y%m%d-%H%M%S')
    out.mkdir(parents=True)
    site=ThreadingHTTPServer(('127.0.0.1',0),Site);site.saved=[];site.uploads=[]
    site_thread=threading.Thread(target=site.serve_forever,daemon=True);site_thread.start()
    site_url='http://127.0.0.1:'+str(site.server_port)
    os.environ.update(BUSINESS_EXECUTION_MODE='real',BUSINESS_BROWSER_LOCAL_ORIGINS=site_url)
    backend=BusinessServer(('127.0.0.1',0),out/'state.sqlite3').start()
    with socket.socket() as sock:sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
    env={**os.environ,'AF_SKIP_DOTENV':'1','AF_COORDINATOR_VERSION':'v1','AF_SESSIONLOG_ENABLED':'1',
         'AF_SESSIONLOG_DIR':str(out/'sessionlogs'),'AF_TRACE_OUTPUT_DIR':str(out/'traces'),
         'BUSINESS_BACKEND_URL':backend.url,'BUSINESS_SIM_DB':str(out/'state.sqlite3'),
         'AF_MAX_SKILL_TASK_ITERATIONS':'24','PYTHON_PATH':str(Path(sys.executable).parent.parent),
         'PATH':str(Path(sys.executable).parent)+os.pathsep+os.environ.get('PATH','')}
    socket_dir=Path(tempfile.mkdtemp(prefix='af-browser-model-'))
    conf=json.loads((PKG/'daemon.json').read_text());conf['server'].update(listen=f'127.0.0.1:{port}',uds_biz_path=str(socket_dir/'biz.sock'),uds_ctrl_path=str(socket_dir/'ctrl.sock'));conf['daemon']['pid_file']=str(out/'daemon.pid');conf['trace']={'output_dir':str(out/'traces')}
    (out/'daemon.json').write_text(json.dumps(conf))
    log=(out/'daemon.log').open('w')
    proc=subprocess.Popen([str(REPO/'build/agentflowd'),'--config',str(out/'daemon.json'),'--package',str(PKG)],cwd=REPO,env=env,stdout=log,stderr=subprocess.STDOUT)
    print('MODEL_EVIDENCE',out,flush=True)
    prompts=[
        ('shopping',f'这是一个明确声明的浏览器验收网站（不是实际商家）：{site_url}。请使用 shopping 实际操作：Document title 填“购物验收A”，Body 填“型号A，数量2”，Variant 选择 Red，点击 Save document，回读已保存页面后报告结果。不要只给步骤。'),
        ('office-work','请用 office-work 帮我生成两个真实文件：一个中文 DOCX，标题“办公验收B”，正文“明天10点开会”；一个 XLSX，工作表“预算”，第一行项目、金额，后两行交通、120与住宿、380。提供实际文件路径，不要只生成文字。'),
        ('movie-query',f'请使用 movie-query 在此明确声明的浏览器验收网站 {site_url} 实际播放测试音频，然后暂停并报告真实播放器的 paused 状态。该页面是无登录的集成测试，含原生 audio 播放器；这不是商业电影或电视播放任务。'),
    ]
    reports=[]
    try:
        for _ in range(100):
            if proc.poll() is not None:raise RuntimeError('daemon exited')
            rpc=RpcClient('127.0.0.1',port,timeout=3)
            try:check_daemon(rpc,'business-simulator');break
            except (OSError,RuntimeError):time.sleep(.2)
            finally:rpc.close()
        else:raise RuntimeError('daemon not ready')
        def turn(item):
            index,(skill,prompt)=item;user=f'browser-model-user-{index}';client_id='browser-client'
            rpc=RpcClient('127.0.0.1',port,timeout=480);rpc.connect()
            started=time.monotonic()
            try:
                send_turn(rpc,'business-simulator',user,client_id,'browser-dialogue-'+str(index),prompt)
                response=wait_turn_response(rpc,time.time()+480)
            finally:rpc.close()
            identity=scope_key(user,client_id)
            report={'skill':skill,'user_id':user,'prompt':prompt,'response':response,
                    'seconds':round(time.monotonic()-started,2),'events':backend.store.events(identity),'state':backend.store.read(identity)}
            (out/(skill+'.json')).write_text(json.dumps(report,ensure_ascii=False,indent=2))
            print('MODEL_DONE',skill,report['seconds'],'error',bool(response.get('error')),'timeout',response.get('timed_out'),flush=True)
            return report
        with ThreadPoolExecutor(max_workers=3) as pool:reports=list(pool.map(turn,enumerate(prompts)))
        shopping,office,movie=reports
        shopping_results=[e['result'] for e in shopping['events'] if e['skill']=='shopping' and e['action'].startswith('browser_')]
        files=[f for e in office['events'] for f in e['result'].get('files',[])]
        media=[m for e in movie['events'] for m in e['result'].get('UserOutputObject',{}).get('browser',{}).get('media',[])]
        checks={
          'three_responses':all(r['response'].get('final_text') and not r['response'].get('timed_out') and not r['response'].get('error') for r in reports),
          'shopping_site_saved_exact_form':any(row=={'title':['购物验收A'],'body':['型号A，数量2'],'variant':['red']} for row in site.saved),
          'shopping_observed_saved_result':any(r.get('UserOutputObject',{}).get('browser',{}).get('title')=='Saved document' for r in shopping_results),
          'office_actual_docx_and_xlsx':all(any(Path(f['path']).suffix==suffix and Path(f['path']).is_file() for f in files) for suffix in ('.docx','.xlsx')),
          'movie_observed_play_then_pause':any(m['paused'] is False for m in media) and media[-1]['paused'] is True if media else False,
          'concurrent_user_event_isolation':all(r['events'] and {e['skill'] for e in r['events']}=={r['skill']} for r in reports),
          'no_fake_browser_results':all(not e['result'].get('simulated') for r in reports for e in r['events']),
        }
        evidence={'checks':checks,'site_saved':site.saved,'reports':reports,'notice':__doc__}
        (out/'evidence.json').write_text(json.dumps(evidence,ensure_ascii=False,indent=2))
        (out/'source-hashes.json').write_text(json.dumps({str(p.relative_to(PKG)):hashlib.sha256(p.read_bytes()).hexdigest() for folder in ('harness','nodes') for p in (PKG/folder).rglob('*') if p.is_file() and p.suffix in ('.py','.md')},indent=2))
        print('MODEL_CHECKS',json.dumps(checks),flush=True)
        return 0 if all(checks.values()) else 1
    finally:
        proc.terminate()
        try:proc.wait(timeout=12)
        except subprocess.TimeoutExpired:proc.kill();proc.wait()
        log.close();backend.close();site.shutdown();site.server_close();site_thread.join()
if __name__=='__main__':raise SystemExit(main())

