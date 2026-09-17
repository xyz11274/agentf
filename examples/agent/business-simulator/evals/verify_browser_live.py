#!/usr/bin/env python3
"""Live read-only website and historical weather checks. Results are evidence, not guaranteed access."""
from pathlib import Path
from datetime import datetime,timezone
import json,os,sys,tempfile
PKG=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(PKG/'harness'))
from business_sim.services import dispatch
from business_sim.state import Store,scope_key
from business_sim.browser import close

def main():
    out=PKG/'outputs/real-world'/datetime.now(timezone.utc).strftime('live-%Y%m%d-%H%M%S')
    out.mkdir(parents=True)
    os.environ['BUSINESS_EXECUTION_MODE']='real'
    os.environ.pop('BUSINESS_BACKEND_URL',None)
    store=Store(out/'state.sqlite3');scope=scope_key('live-web-verification','read-only')
    checks=[]
    def run(skill,args,label):
        answer=dispatch(skill,args,store=store,scope=scope)
        checks.append({'check':label,'skill':skill,'args':args,'result':answer})
        (out/'evidence.json').write_text(json.dumps(checks,ensure_ascii=False,indent=2))
        page=answer.get('UserOutputObject',{}).get('browser',{})
        print(json.dumps({'check':label,'status':answer['status'],'title':page.get('title'),'url':page.get('url'),'elements':len(page.get('elements',[])),'error':answer.get('error')},ensure_ascii=False),flush=True)
        return answer
    try:
        for label,url in [('retailer-jd','https://search.jd.com/Search?keyword=%E5%8F%B0%E7%81%AF'),
                          ('retailer-ikea','https://www.ikea.cn/cn/zh/search/?q=%E5%8F%B0%E7%81%AF')]:
            answer=run('shopping',{'action':'browser_open','url':url},label)
            page=answer.get('UserOutputObject',{}).get('browser',{})
            items=[e for e in page.get('elements',[]) if e.get('href') and ('/p/' in e['href'] or 'item.jd.com' in e['href'])]
            if items:
                run('shopping',{'action':'browser_click','ref':items[0]['ref']},label+'-product-detail')
        answer=run('movie-query',{'action':'browser_open','url':'https://www.w3.org/2010/05/video/mediaevents.html'},'public-html5-player')
        page=answer.get('UserOutputObject',{}).get('browser',{})
        videos=[e for e in page.get('elements',[]) if e['tag']=='video']
        if videos:
            answer=run('movie-query',{'action':'browser_media','ref':videos[0]['ref'],'media_command':'play'},'public-player-play')
            videos=[e for e in answer.get('UserOutputObject',{}).get('browser',{}).get('elements',[]) if e['tag']=='video']
            if videos:run('movie-query',{'action':'browser_media','ref':videos[0]['ref'],'media_command':'pause'},'public-player-pause')
        run('office-work',{'action':'browser_open','url':'https://docs.google.com/document/'},'office-login-readiness')
        run('web-search',{'action':'weather','location':'北京','date':'2023-09-05'},'historical-weather-live')
    finally:
        close(store)
    print('EVIDENCE',out/'evidence.json',flush=True)
if __name__=='__main__':main()

