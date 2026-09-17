"""Real Chromium interaction shared by business skills, with isolated user state.

Playwright objects live on one dedicated thread. Every action returns a fresh
page observation; the caller must distinguish a successful click from a
successful business operation. No generated page or benchmark fixture is used.
"""
from concurrent.futures import ThreadPoolExecutor
import atexit
import hashlib
import ipaddress
import json
import os
from pathlib import Path
import socket
import sys
import threading
import uuid
from urllib.parse import urlsplit

from .live_information import result

PACKAGE = Path(__file__).resolve().parents[2]
RUNTIME = PACKAGE / 'outputs/browser-runtime'
ACTIONS = ['browser_open','browser_observe','browser_wait','browser_click','browser_fill','browser_select',
           'browser_press','browser_scroll','browser_back','browser_screenshot',
           'browser_download','browser_upload','browser_media','browser_close']
FIELDS = {
    'url':{'type':'string','description':'用户任务相关的真实 http/https 地址'},
    'ref':{'type':'string','description':'最近一次页面观察返回的元素引用；不能猜测'},
    'text':{'type':'string','description':'输入或按键；密码不要写入普通文档'},
    'option':{'type':'string','description':'下拉框选项值'},
    'wait_ms':{'type':'integer','minimum':100,'maximum':10000},
    'wait_text':{'type':'string','description':'等待网页出现的文本，例如商品标题或保存成功提示'},
    'scroll_y':{'type':'integer','minimum':-5000,'maximum':5000},
    'file_path':{'type':'string','description':'当前用户工具生成的文件，供上传'},
    'media_command':{'type':'string','enum':['play','pause','seek','status']},
    'position_seconds':{'type':'number','minimum':0},
}
_ENGINES = {}
_LOCK = threading.Lock()


def installed():
    return (RUNTIME/'playwright').is_dir()


def executable():
    configured=os.environ.get('BUSINESS_BROWSER_EXECUTABLE')
    if configured:
        return configured
    roots=[PACKAGE/'outputs/browsers',Path.home()/'.cache/ms-playwright']
    for root in roots:
        choices=sorted(root.glob('chromium-*/chrome-linux*/chrome'),reverse=True)
        if choices:
            return str(choices[0])
    return None


class BrowserEngine:
    def __init__(self, store):
        self.store=store
        self.pool=ThreadPoolExecutor(max_workers=1,thread_name_prefix='business-browser')
        self.playwright=None
        self.browser=None
        self.contexts={}
        self.pages={}
        self.hosts={}
        self.closed=False
        self.allowed_origins=set(filter(None,(','.join(filter(None,[os.environ.get('BUSINESS_BROWSER_ALLOWED_ORIGINS',''),os.environ.get('BUSINESS_BROWSER_LOCAL_ORIGINS','')]))).split(',')))

    def safe_url(self,url):
        parsed=urlsplit(url)
        if parsed.scheme not in ('https','http') or not parsed.hostname or parsed.username or parsed.password:
            return False
        origin=f'{parsed.scheme}://{parsed.netloc}'
        if origin in self.allowed_origins:
            return True
        host=parsed.hostname
        if host in self.hosts:
            return self.hosts[host]
        try:
            addresses={row[4][0] for row in socket.getaddrinfo(host,None)}
            allowed=bool(addresses) and all(ipaddress.ip_address(ip).is_global for ip in addresses)
        except (OSError,ValueError):
            allowed=False
        self.hosts[host]=allowed
        return allowed

    def start(self):
        if self.browser:
            return
        if not installed():
            raise RuntimeError('browser_dependencies_missing; run scripts/setup_browser.py')
        sys.path.insert(0,str(RUNTIME))
        from playwright.sync_api import sync_playwright
        self.playwright=sync_playwright().start()
        settings={'headless':True,'args':['--disable-dev-shm-usage','--autoplay-policy=no-user-gesture-required']}
        libs=PACKAGE/'outputs/browser-libs'
        child_env={k:v for k,v in os.environ.items() if k in ('PATH','HOME','LANG','LC_ALL','TZ','DISPLAY','XDG_RUNTIME_DIR')}
        child_env['LD_LIBRARY_PATH']=str(libs)+':'+os.environ.get('LD_LIBRARY_PATH','')
        settings['env']=child_env
        if os.geteuid()==0:
            settings['args'].append('--no-sandbox')
        if executable():
            settings['executable_path']=executable()
        proxy=os.environ.get('BUSINESS_BROWSER_PROXY') or os.environ.get('HTTPS_PROXY') or os.environ.get('https_proxy')
        if proxy:
            parsed=urlsplit(proxy)
            settings['proxy']={'server':f'{parsed.scheme}://{parsed.hostname}:{parsed.port}',
                               'bypass':'localhost,127.0.0.1,::1'}
            if parsed.username:
                settings['proxy'].update(username=parsed.username,password=parsed.password or '')
        self.browser=self.playwright.chromium.launch(**settings)

    def entry(self,scope,skill):
        self.start()
        owner=self.store.owner_scope(scope)
        if owner not in self.contexts:
            directory=self.store.path.parent/'browser'/hashlib.sha256(owner.encode()).hexdigest()
            directory.mkdir(parents=True,exist_ok=True,mode=0o700)
            auth=directory/'auth.json'
            options={'viewport':{'width':1440,'height':1000},'accept_downloads':True,'locale':'zh-CN'}
            if auth.is_file():
                options['storage_state']=str(auth)
            ctx=self.browser.new_context(**options)
            # Browser content cannot use this service to read local files or LAN APIs.
            ctx.route('**/*',lambda route: route.continue_() if self.safe_url(route.request.url) else route.abort())
            self.contexts[owner]=(ctx,auth)
        ctx,auth=self.contexts[owner]
        key=(str(scope),skill)
        if key not in self.pages or self.pages[key]['page'].is_closed():
            page=ctx.new_page()
            page.set_default_timeout(15000)
            page.set_default_navigation_timeout(45000)
            entry={'page':page,'refs':{},'auth':auth,'owner':owner,'context':ctx}
            page.on('popup',lambda popup: entry.update(page=popup,refs={}))
            self.pages[key]=entry
        return self.pages[key]

    def snapshot(self,entry):
        page=entry['page']
        refs={};elements=[]
        generation=uuid.uuid4().hex[:10]
        for frame_index,frame in enumerate(page.frames[:12]):
            try:
                rows=frame.evaluate('''prefix => {
                  const all=[...document.querySelectorAll('a,button,input,textarea,select,[role="button"],[role="link"],[contenteditable="true"],video,audio')];
                  return all.filter(e=>e.getClientRects().length && getComputedStyle(e).visibility!=='hidden').slice(0,150).map((e,i)=>{
                    const ref=prefix+'-'+i; e.setAttribute('data-af-ref',ref);
                    const owner=e.labels?.[0]?.cloneNode(true);
                    if(owner)owner.querySelectorAll('input,textarea,select').forEach(x=>x.remove());
                    const label=e.getAttribute('aria-label') || owner?.textContent?.trim() || e.innerText || e.getAttribute('placeholder') || e.getAttribute('title') || e.getAttribute('name') || '';
                    return {ref,tag:e.tagName.toLowerCase(),role:e.getAttribute('role'),type:e.getAttribute('type'),label:label.slice(0,220),
                      value:e.type==='password'?undefined:(e.value||'').slice(0,150),href:e.href||undefined,
                      options:e.tagName==='SELECT'?[...e.options].map(o=>({value:o.value,label:o.label})):undefined};
                  });
                }''',f'{generation}-{frame_index}')
                for row in rows:
                    refs[row['ref']]=frame.locator('[data-af-ref="'+row['ref']+'"]')
                    elements.append(row)
            except Exception:
                continue  # Detached/cross-navigation frames are observed on the next call.
        entry['refs']=refs
        text=page.locator('body').inner_text(timeout=10000)[:16000]
        media=page.locator('video,audio').evaluate_all('(xs)=>xs.map(x=>({tag:x.tagName.toLowerCase(),paused:x.paused,current_time:x.currentTime,duration:Number.isFinite(x.duration)?x.duration:null,ready_state:x.readyState,source:x.currentSrc}))')
        return {'url':page.url,'title':page.title(),'text':text,'elements':elements,'media':media,
                'observation_id':generation,'page_content_is_untrusted':True}

    def artifact(self,scope,name):
        root=self.store.path.parent/'artifacts'/str(scope)
        root.mkdir(parents=True,exist_ok=True)
        return root/Path(name).name

    def operate(self,scope,skill,args):
        entry=self.entry(scope,skill);page=entry['page'];action=args['action'];file=None
        locator=None
        if action in ('browser_click','browser_fill','browser_select','browser_press','browser_download','browser_upload','browser_media'):
            locator=entry['refs'].get(args.get('ref'))
            if locator is None:
                return result('元素引用不存在或已过期，请重新观察页面。',status='needs_input',missing_fields=['ref'])
        if action=='browser_open':
            if not self.safe_url(args.get('url','')):
                return result('仅能打开公开HTTP/HTTPS网站或管理员配置的业务地址。',status='failed',error='url_not_allowed')
            page.goto(args['url'],wait_until='domcontentloaded')
        elif action=='browser_wait':
            if args.get('wait_text'):
                page.get_by_text(args['wait_text'],exact=False).first.wait_for(
                    state='visible',timeout=args.get('wait_ms',5000))
            else:
                page.wait_for_timeout(args.get('wait_ms',1500))
        elif action=='browser_click':
            locator.click()
        elif action=='browser_fill':
            locator.fill(args.get('text',''))
        elif action=='browser_select':
            locator.select_option(args['option'])
        elif action=='browser_press':
            locator.press(args.get('text','Enter'))
        elif action=='browser_scroll':
            page.mouse.wheel(0,args.get('scroll_y',700))
        elif action=='browser_back':
            page.go_back(wait_until='domcontentloaded')
        elif action=='browser_screenshot':
            file=self.artifact(scope,'browser-'+uuid.uuid4().hex+'.png')
            page.screenshot(path=str(file),full_page=False)
        elif action=='browser_download':
            with page.expect_download(timeout=30000) as download:
                locator.click()
            received=download.value
            file=self.artifact(scope,'download-'+uuid.uuid4().hex+'-'+Path(received.suggested_filename).name)
            received.save_as(str(file))
        elif action=='browser_upload':
            path=Path(args['file_path']).resolve()
            allowed=[(self.store.path.parent/'artifacts'/str(s)).resolve() for s in (scope,entry['owner'])]
            if not path.is_file() or not any(path.is_relative_to(root) for root in allowed):
                return result('只能上传当前用户工具生成的文件。',status='failed',error='file_not_owned')
            if locator.evaluate('(e)=>e.tagName==="INPUT" && e.type==="file"'):
                locator.set_input_files(str(path))
            else:
                with page.expect_file_chooser() as chosen:
                    locator.click()
                chosen.value.set_files(str(path))
        elif action=='browser_media':
            if locator.evaluate('(e)=>!["VIDEO","AUDIO"].includes(e.tagName)'):
                return result('该元素不是播放器，请重新观察页面。',status='failed')
            op=args.get('media_command','status')
            if op=='play':locator.evaluate('(e)=>e.play()')
            elif op=='pause':locator.evaluate('(e)=>e.pause()')
            elif op=='seek':locator.evaluate('(e,t)=>{e.currentTime=t}',args['position_seconds'])
        elif action=='browser_close':
            entry['context'].storage_state(path=str(entry['auth']))
            entry['auth'].chmod(0o600)
            page.close()
            return result('浏览器页面已关闭。',{'closed':True},execution='executed',source='chromium')
        elif action!='browser_observe':
            return result('不支持的浏览器操作。',status='unsupported')
        page=entry['page']
        try:page.wait_for_load_state('domcontentloaded',timeout=15000)
        except Exception:pass
        observation=self.snapshot(entry)
        entry['context'].storage_state(path=str(entry['auth']))
        entry['auth'].chmod(0o600)
        files=[]
        if file:
            files=[{'path':str(file.resolve()),'bytes':file.stat().st_size,'sha256':hashlib.sha256(file.read_bytes()).hexdigest()}]
        return result('浏览器操作已执行；请依据新页面确认业务结果。',{'browser':observation,'files':files},
                      execution='executed',source='chromium',files=files,business_outcome_verified=False)

    def call(self,scope,skill,args):
        try:
            future=self.pool.submit(self.operate,scope,skill,args)
            return future.result(timeout=115)
        except Exception as exc:
            # Cancel only queued work. A started write may have reached the site.
            cancelled=future.cancel() if 'future' in locals() else True
            # Do not expose browser cookies, proxy credentials or filled secrets in errors.
            return result('浏览器操作结果未确认，请先重新观察页面；不要直接重复提交。登录或验证须由用户完成。',status='failed',
                          error=type(exc).__name__,source='chromium',execution='not_executed' if cancelled else 'not_verified',retryable=False)

    def close(self):
        if self.closed:return
        self.closed=True
        def shutdown():
            if self.browser:self.browser.close()
            if self.playwright:self.playwright.stop()
        self.pool.submit(shutdown).result(timeout=20)
        self.pool.shutdown(wait=True)


def execute(store,scope,skill,args):
    if not installed():
        return result('请运行 scripts/setup_browser.py 安装浏览器组件。',status='unsupported',missing_capabilities=['playwright_runtime'])
    key=str(store.path.resolve())
    with _LOCK:
        if key not in _ENGINES:
            _ENGINES[key]=BrowserEngine(store)
        engine=_ENGINES[key]
    return engine.call(scope,skill,args)


def close(store=None):
    with _LOCK:
        keys=[str(store.path.resolve())] if store else list(_ENGINES)
        engines=[_ENGINES.pop(k) for k in keys if k in _ENGINES]
    for engine in engines:
        try:engine.close()
        except Exception:pass


atexit.register(close)
