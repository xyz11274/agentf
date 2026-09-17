"""A versioned, user-owned corpus of imported sources; no bundled benchmark answers."""
from datetime import datetime, timezone
import hashlib, re
from .live_information import result

def import_page(store,scope,args):
    from .browser import execute
    page=execute(store,scope,'knowledge-base',{'action':'browser_open','url':args.get('url','')})
    if page['status']!='succeeded':return page
    observation=page['UserOutputObject']['browser']
    if not observation['text'].strip():return result('页面没有可提取正文。',status='failed')
    return {'title':args.get('title') or observation['title'],'content':observation['text'],
            'source_url':observation['url'],'source_kind':'observed_webpage',
            'observation_id':observation['observation_id'],'content_may_be_truncated':len(observation['text'])>=16000}

def execute(state,args,prepared=None):
    documents=state.setdefault('knowledge_documents',{})
    action=args['action']
    if action in ('import_url','import_text'):
        if prepared is not None and 'status' in prepared:return prepared
        item=prepared if prepared is not None else {
            'title':args.get('title','用户提供资料'),'content':args.get('content',''),
            'source_url':args.get('url',''),'source_kind':'user_provided'}
        if not item.get('content'):return result('需要资料正文。',status='needs_input',missing_fields=['content'])
        key=args.get('key') or hashlib.sha256((item['source_url'] or item['title']).encode()).hexdigest()
        old=documents.get(key,{})
        digest=hashlib.sha256(item['content'].encode()).hexdigest()
        if old.get('sha256')==digest:return result('资料内容未变化。',{'document':{k:v for k,v in old.items() if k!='content'}},duplicate=True)
        revisions=old.get('revisions',[])+([{k:v for k,v in old.items() if k!='revisions'}] if old else [])
        item={**item,'key':key,'version':old.get('version',0)+1,'sha256':digest,'revisions':revisions,
              'imported_at':datetime.now(timezone.utc).isoformat(),'content_is_untrusted':True}
        documents[key]=item
        return result('资料已导入用户知识库；保留来源和版本。',{'document':{k:v for k,v in item.items() if k not in ('content','revisions')}})
    if action=='delete_source':
        if args.get('key') not in documents:return result('资料不存在。',status='not_found')
        del documents[args['key']]
        return result('已删除该资料及其索引。')
    if action=='list_sources':
        return result('已导入资料目录。',{'sources':[{k:v for k,v in d.items() if k not in ('content','revisions')} for d in documents.values()]})
    query=args.get('query','').strip().lower()
    if not query:return result('请输入查询词。',status='needs_input',missing_fields=['query'])
    tokens=set(re.findall(r'[a-z0-9]+',query))
    for run in re.findall(r'[\u4e00-\u9fff]+',query):
        tokens.update(run[i:i+2] for i in range(len(run)-1))
        if len(run)==1:tokens.add(run)
    rows=[]
    for item in documents.values():
        content=item['content'];text=(item['title']+'\n'+content).lower()
        matched=[t for t in tokens if t in text]
        score=len(matched)+10*(query in text)
        if score:
            position=min((content.lower().find(t) for t in matched if t in content.lower()),default=0)
            rows.append({'key':item['key'],'title':item['title'],'source_url':item['source_url'],
                         'source_kind':item['source_kind'],'version':item['version'],'sha256':item['sha256'],
                         'excerpt':content[max(0,position-150):position+1800],'score':score,'content_is_untrusted':True})
    rows.sort(key=lambda x:-x['score'])
    return result('用户知识库检索结果。' if rows else '已导入的知识库中未找到资料；不能推断账号权益。',
                  {'results':rows[:8],'source_count':len(documents)},status='succeeded' if rows else 'not_found')

