"""Persist sourced film lists and resolve references without inventing a catalog."""
from urllib.parse import urlsplit
from .live_information import result

def select(state,args):
    items=state.get('last_movies',[])
    if args.get('index'):
        index=args['index']-1
        return items[index] if 0<=index<len(items) else None
    title=args.get('title') or args.get('query')
    matches=[m for m in items if m['title']==title or m.get('id')==title]
    return matches[0] if len(matches)==1 else None

def execute(state,args):
    action=args['action']
    if action=='save_recommendations':
        items=args.get('recommendations',[])
        if not items:return result('需要影片及其来源列表。',status='needs_input',missing_fields=['recommendations'])
        if any(urlsplit(i.get('source_url','')).scheme not in ('http','https') for i in items):
            return result('每部影片都需要实际网页来源。',status='failed')
        excluded=set(args.get('exclude',[]))
        if any(i['title'] in excluded for i in items):
            return result('片单包含用户排除的影片，请修正。',status='failed')
        state['last_movies']=items[:args.get('count',10)]
        return result('已保存本轮片单，后续序号引用该列表；资料须以来源为准。',
                      {'aiMovieData':{'list':state['last_movies']}},operation_type='200002')
    if action=='query':
        return result('已保存的影片列表和最近执行状态。',{'aiMovieData':{'list':state.get('last_movies',[])},
                      'player':state.get('player',{}),'requires_live_player_observation':True})
    item=select(state,args)
    if not item:
        return result('未找到指定序号或片名；请从当前片单选择。',status='not_found')
    return result('已读取指定影片的已保存资料。',{'movie':item,'aiMovieData':{'list':[item]}},
                  operation_type='200003')

def playback(store,scope,args):
    from .browser_receipts import execute as browse
    state=store.read(scope)
    item=select(state,args)
    if args['action']=='play':
        url=args.get('url') or (item or {}).get('play_url')
        if not url:
            return result('需要合法播放页地址；影片资料页不代表可播放。请打开用户指定平台并选择其播放器。',
                          {'selected_movie':item},status='needs_input',missing_fields=['play_url'],
                          execution='not_executed')
        opened=browse(store,scope,'movie-query',{'action':'browser_open','url':url})
    else:
        opened=browse(store,scope,'movie-query',{'action':'browser_observe'})
    if opened['status']!='succeeded':return opened
    players=[e for e in opened['UserOutputObject']['browser']['elements'] if e['tag'] in ('video','audio')]
    if len(players)!=1:
        return result('页面没有唯一可操作的播放器；需要完成平台登录、选片或通过 browser_media 指定元素。',
                      opened['UserOutputObject'],status='needs_input',execution='not_verified')
    return browse(store,scope,'movie-query',{'action':'browser_media','ref':players[0]['ref'],
                                            'media_command':'pause' if args['action']=='pause' else 'play',
                                            'request_id':args.get('request_id') or players[0]['ref']})

