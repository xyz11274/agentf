"""Real Chromium integration; the local site is explicitly a test fixture."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from io import BytesIO
import hashlib, json, sys, threading, uuid, wave
from pathlib import Path
from urllib.parse import parse_qs
import pytest

PKG=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(PKG/'harness'))
from business_sim import browser
from business_sim.contracts import SERVICES
from business_sim.real_services import save_artifact
from business_sim.services import dispatch
from business_sim.state import Store,scope_key

PAGE='''<!doctype html><meta charset="utf-8"><title>Browser integration test</title>
<h1>Declared test website</h1><form method="post" action="/save">
<label>Document title<input name="title"></label><label>Body<textarea name="body"></textarea></label>
<label>Variant<select name="variant"><option value="blue">Blue</option><option value="red">Red</option></select></label>
<button>Save document</button></form><a href="/download">Download result</a>
<input type="file" aria-label="Upload file" onchange="fetch('/upload',{method:'POST',body:this.files[0]}).then(r=>r.text()).then(t=>document.querySelector('#upload').textContent=t)"><p id="upload"></p>
<audio controls src="/sound.wav" aria-label="Test audio"></audio>'''

class Site(BaseHTTPRequestHandler):
    def log_message(self,*args):pass
    def reply(self,content,mime='text/html; charset=utf-8'):
        if isinstance(content,str):content=content.encode()
        self.send_response(200);self.send_header('Content-Type',mime);self.send_header('Content-Length',str(len(content)))
        if 'site_owner=' not in self.headers.get('Cookie',''):
            self.send_header('Set-Cookie','site_owner='+uuid.uuid4().hex+'; Path=/; HttpOnly')
        if self.path=='/download':self.send_header('Content-Disposition','attachment; filename="report.txt"')
        self.end_headers();self.wfile.write(content)
    def do_GET(self):
        if self.path=='/download':return self.reply('Actual downloaded contents','text/plain')
        if self.path=='/sound.wav':
            out=BytesIO()
            with wave.open(out,'wb') as w:
                w.setnchannels(1);w.setsampwidth(2);w.setframerate(8000);w.writeframes(b'\x00\x00'*8000*20)
            return self.reply(out.getvalue(),'audio/wav')
        if self.path=='/identity':return self.reply('<body>'+self.headers.get('Cookie','')+'</body>')
        self.reply(PAGE)
    def do_POST(self):
        body=self.rfile_read()
        if self.path=='/upload':
            self.server.uploads.append(body);return self.reply('Uploaded '+hashlib.sha256(body).hexdigest(),'text/plain')
        data=parse_qs(body.decode(),keep_blank_values=True);self.server.saved.append(data)
        self.reply('<title>Saved document</title><h1>Saved</h1><pre>'+json.dumps(data)+'</pre><a href="/">Back</a>')
    def rfile_read(self):
        return self.rfile.read(int(self.headers['Content-Length']))

@pytest.fixture(scope='module')
def site():
    server=ThreadingHTTPServer(('127.0.0.1',0),Site);server.saved=[];server.uploads=[]
    thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
    yield server,'http://127.0.0.1:'+str(server.server_port)
    server.shutdown();server.server_close();thread.join()

@pytest.fixture
def env(tmp_path,monkeypatch,site):
    if not browser.installed() or not browser.executable():pytest.skip('run scripts/setup_browser.py')
    monkeypatch.setenv('BUSINESS_EXECUTION_MODE','real')
    monkeypatch.delenv('BUSINESS_BACKEND_URL',raising=False)
    monkeypatch.setenv('BUSINESS_BROWSER_LOCAL_ORIGINS',site[1])
    store=Store(tmp_path/'state.sqlite3')
    yield store,scope_key('browser-owner','chat'),site[1]
    browser.close(store)

def call(env,skill='shopping',**args):
    answer=dispatch(skill,args,store=env[0],scope=env[1])
    assert answer['status']=='succeeded',answer
    assert answer['simulated'] is False
    return answer

def ref(answer,label):
    return next(e['ref'] for e in answer['UserOutputObject']['browser']['elements'] if e['label']==label)

@pytest.mark.parametrize('skill',list(SERVICES))
def test_each_business_skill_reaches_real_browser(env,skill):
    observed=call(env,skill,action='browser_open',url=env[2])
    assert observed['source']=='chromium'
    assert 'Declared test website' in observed['UserOutputObject']['browser']['text']
    assert len(observed['UserOutputObject']['browser']['elements'])>=7
    assert observed['business_outcome_verified'] is False

def test_fill_select_submit_and_explicit_retry_changes_site_once(env,site):
    before=len(site[0].saved)
    a=call(env,'office-work',action='browser_open',url=env[2]);old=ref(a,'Document title')
    a=call(env,'office-work',action='browser_fill',ref=old,text='Customer report')
    stale=dispatch('office-work',{'action':'browser_fill','ref':old,'text':'wrong'},store=env[0],scope=env[1])
    assert stale['status']=='needs_input'
    a=call(env,'office-work',action='browser_fill',ref=ref(a,'Body'),text='Actual document body')
    a=call(env,'office-work',action='browser_select',ref=ref(a,'Variant'),option='red')
    args={'action':'browser_click','ref':ref(a,'Save document'),'request_id':'submit-once'}
    submitted=call(env,'office-work',**args);replay=call(env,'office-work',**args)
    assert replay['replayed'] and len(site[0].saved)==before+1
    assert site[0].saved[-1]=={'title':['Customer report'],'body':['Actual document body'],'variant':['red']}
    assert submitted['UserOutputObject']['browser']['title']=='Saved document'

def test_download_upload_screenshot_and_actual_media_state(env,site):
    a=call(env,'movie-query',action='browser_open',url=env[2])
    a=call(env,'movie-query',action='browser_media',ref=ref(a,'Test audio'),media_command='play')
    assert a['UserOutputObject']['browser']['media'][0]['paused'] is False
    a=call(env,'movie-query',action='browser_media',ref=ref(a,'Test audio'),media_command='pause')
    assert a['UserOutputObject']['browser']['media'][0]['paused'] is True
    a=call(env,'movie-query',action='browser_download',ref=ref(a,'Download result'))
    path=Path(a['files'][0]['path']);assert path.read_text()=='Actual downloaded contents'
    call(env,'movie-query',action='browser_upload',ref=ref(a,'Upload file'),file_path=str(path))
    assert path.read_bytes() in site[0].uploads
    a=call(env,'movie-query',action='browser_screenshot')
    assert Path(a['files'][0]['path']).read_bytes().startswith(b'\x89PNG')

def test_user_cookie_persistence_isolation_and_upload_ownership(env):
    call(env,action='browser_open',url=env[2])
    first=call(env,action='browser_open',url=env[2]+'/identity')['UserOutputObject']['browser']['text']
    same=(env[0],scope_key('browser-owner','second-chat'),env[2])
    assert call(same,action='browser_open',url=env[2]+'/identity')['UserOutputObject']['browser']['text']==first
    other=(env[0],scope_key('another-owner','chat'),env[2])
    call(other,action='browser_open',url=env[2])
    assert call(other,action='browser_open',url=env[2]+'/identity')['UserOutputObject']['browser']['text']!=first
    secret=save_artifact(other[0],other[1],'owned.txt',b'other user','text/plain')
    a=call(env,action='browser_open',url=env[2])
    outcome=dispatch('shopping',{'action':'browser_upload','ref':ref(a,'Upload file'),'file_path':secret['path']},store=env[0],scope=env[1])
    assert outcome['status']=='failed' and outcome['error']=='file_not_owned'
    browser.close(env[0])
    assert call(env,action='browser_open',url=env[2]+'/identity')['UserOutputObject']['browser']['text']==first

def test_arbitrary_local_services_and_file_urls_are_not_websites(env):
    for url in ('file:///etc/passwd','http://127.0.0.1:1','http://169.254.169.254/'):
        outcome=dispatch('third-party',{'action':'browser_open','url':url},store=env[0],scope=env[1])
        assert outcome['status']=='failed' and outcome['error']=='url_not_allowed'


def test_same_skill_parallel_branches_keep_pages_refs_and_receipts_separate(env):
    from business_sim.task_context import task_context
    with task_context('dispatch-a'):
        first=call(env,'office-work',action='browser_open',url=env[2])
    with task_context('dispatch-b'):
        second=call(env,'office-work',action='browser_open',url=env[2])
        call(env,'office-work',action='browser_fill',ref=ref(second,'Document title'),text='branch-b',request_id='fill-1')
    with task_context('dispatch-a'):
        result=call(env,'office-work',action='browser_fill',ref=ref(first,'Document title'),text='branch-a',request_id='fill-1')
        assert next(e['value'] for e in result['UserOutputObject']['browser']['elements'] if e['label']=='Document title')=='branch-a'
    with task_context('dispatch-b'):
        result=call(env,'office-work',action='browser_observe')
        assert next(e['value'] for e in result['UserOutputObject']['browser']['elements'] if e['label']=='Document title')=='branch-b'


def test_idle_branch_pages_are_reclaimed_without_evicting_active_branch(env,monkeypatch):
    from business_sim.task_context import task_context
    with task_context('idle'):
        call(env,action='browser_open',url=env[2])
    engine=browser._ENGINES[str(env[0].path.resolve())]
    idle_key=(str(env[1]),'shopping','idle')
    engine.pages[idle_key]['last_used']-=3600
    monkeypatch.setenv('BUSINESS_BROWSER_MAX_PAGES','1')
    with task_context('active'):
        call(env,action='browser_open',url=env[2])
    assert idle_key not in engine.pages and len(engine.pages)==1
    with task_context('overflow'):
        answer=dispatch('shopping',{'action':'browser_open','url':env[2]},store=env[0],scope=env[1])
        assert answer['status']=='failed'
    with task_context('active'):
        call(env,action='browser_observe')
