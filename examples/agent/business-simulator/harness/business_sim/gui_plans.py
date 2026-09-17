"""Parameter-preserving application plans, distinct from device execution."""
import copy
import uuid
from urllib.parse import urlencode
from .live_information import result

OPERATIONS = ['open','search','play','navigate','setting','order','recharge','send',
              'favorite','unfavorite','install','uninstall','checkin','comment','taxi',
              'download','history','playlist','like','page','query_balance','login']


def build(args):
    app, op = args.get('app'), args.get('operation')
    if not app or op not in OPERATIONS:
        raise ValueError('请指定应用和支持的操作')
    target=args.get('title') or args.get('query')
    required={'search':['query'],'play':['title'],'navigate':['destination'],'taxi':['destination'],
              'setting':['key','value'],'order':['query'],'recharge':['amount'],'send':['content'],
              'comment':['content'],'install':['query'],'uninstall':['query'],'download':['query']}.get(op,[])
    missing=[key for key in required if args.get(key) in (None,'')]
    if missing:
        raise ValueError('请补充：'+', '.join(missing))
    steps=[f'打开{app}']
    unresolved=[]
    def find_target():
        if target:
            steps.extend([f'搜索：{target}',f'核对名称并进入“{target}”详情'])
        else:
            steps.append('进入用户当前正在查看或播放的内容')
            unresolved.append('当前内容身份，需要页面观察确认')
    if op=='open':
        if args.get('page'):
            steps.append('进入页面：'+args['page'])
    elif op=='search':
        steps.append('搜索：'+args['query'])
    elif op=='page':
        steps.append('进入页面：'+(args.get('page') or args.get('query') or '用户指定页面'))
    elif op=='play':
        find_target()
        steps.extend([f'选择第{args["episode"]}集' if args.get('episode') else '选择对应内容','开始播放并确认播放状态'])
    elif op in ('favorite','unfavorite','like','comment','playlist'):
        find_target()
        if op in ('favorite','playlist'):
            steps.append('点击收藏'+('，选择歌单：'+args['playlist'] if args.get('playlist') else ''))
            steps.append('检查收藏状态或歌单中已出现该内容')
        elif op=='unfavorite':
            steps.extend(['取消收藏','确认收藏状态已取消'])
        elif op=='like':
            steps.extend(['点击喜欢或点赞','确认当前点赞状态'])
        else:
            steps.extend(['打开评论区','填写评论正文：'+args['content'],'核对作品和正文后发布','检查评论发布结果'])
    elif op in ('install','uninstall','download'):
        find_target()
        if op=='uninstall':
            steps.extend(['进入应用管理，核对包名与应用名','选择卸载并确认','检查应用已卸载'])
        elif op=='install':
            steps.extend(['核对开发者及应用身份','发起安装，处理必要权限提示','等待安装完成并确认可打开'])
        else:
            steps.extend(['选择下载，核对文件或内容','确认保存位置并等待完成','检查下载记录和实际文件'])
    elif op=='checkin':
        steps.extend(['进入'+(args.get('page') or '签到/每日奖励页面'),'查看今日是否已签到，已签到则停止','点击签到或领取奖励','检查今日签到记录和领取结果'])
    elif op=='history':
        steps.append('进入'+(args.get('page') or '观看/播放历史页面'))
        if args.get('index'):
            steps.extend([f'核对历史中的第{args["index"]}项','选择继续播放并确认进度'])
    elif op=='query_balance':
        steps.extend(['进入我的账户或话费余额页面','读取余额、更新时间及账户身份'])
    elif op=='login':
        steps.extend(['进入登录页面','由用户完成账号验证','检查登录后的账号身份'])
        unresolved.append('用户登录与验证')
    elif op in ('navigate','taxi'):
        steps.extend(['定位当前位置' if not args.get('origin') or '当前位置' in args['origin'] else '设置起点：'+args['origin'],
                      '搜索目的地：'+args['destination'],'核对目的地名称、地址和地图位置'])
        if not args.get('origin_coordinates') and (not args.get('origin') or '当前位置' in args['origin']):
            unresolved.append('用户实时定位')
        if op=='taxi':
            steps.extend(['进入打车功能','确认上车点：'+(args.get('pickup_point') or '根据实际定位与用户核对'),
                          '确认车型：'+(args.get('vehicle_type') or '由用户选择'), '核对目的地、预估费用和等待时间','确认后发起叫车','查看接单和车辆信息'])
        else:
            if args.get('avoid_congestion'):
                steps.append('开启避免拥堵路线偏好')
            steps.extend(['比较实时预计用时并选择最快路线' if args.get('route_preference')=='fastest' else '比较路线并选择满足约束的方案','确认路线后开始导航'])
    elif op=='setting':
        steps.extend(['进入设置：'+args['key'],'将值修改为：'+args['value'],'读取设置值确认修改'])
    elif op=='recharge':
        steps.extend(['进入充值页面','确认充值号码：'+(args.get('phone_number') or '用户指定号码'),f'选择充值金额：{args["amount"]}元','核对号码、金额和支付方式，确认后提交','检查支付及到账结果'])
        if not args.get('phone_number'):
            unresolved.append('充值号码')
    elif op=='send':
        steps.extend(['进入收件人或会话：'+(args.get('recipient') or '待用户指定'), '填写正文：'+args['content'],'核对收件人与正文，确认后发送','查看发送状态'])
        if not args.get('recipient'):
            unresolved.append('收件人')
    elif op=='order':
        find_target()
        steps.extend(['选择规格：'+(args.get('variant') or '待核对'),f'设置数量：{args.get("quantity",1)}',
                      '确认收货或服务地址：'+(args.get('address') or '待用户指定'),'核对商品、数量、费用、地址后提交订单','查看订单编号与支付状态'])
        if not args.get('address'):
            unresolved.append('订单地址')
    constraints={k:copy.deepcopy(v) for k,v in args.items() if k not in ('action','request_id','operations')}
    plan={'id':'plan-'+uuid.uuid4().hex,'app':app,'operation':op,'steps':steps,'constraints':constraints,
          'unresolved_fields':unresolved,'execution':'planned','device_connected':False,'route_calculated':False,
          'requires_confirmation':op in ('send','comment','order','recharge','taxi','uninstall')}
    if op=='navigate':
        for key in ('origin_coordinates','destination_coordinates'):
            if args.get(key):
                lon,lat=map(float,args[key].split(','))
                if not -180<=lon<=180 or not -90<=lat<=90:
                    raise ValueError('经纬度超出范围')
        coords=args.get('destination_coordinates')
        params={'src':'agentflow','callnative':1}
        if coords:
            params.update(to=f'{coords},{args["destination"]}',mode='car',policy=1 if args.get('avoid_congestion') else 0)
            if args.get('origin_coordinates'):
                params['from']=args['origin_coordinates']
            plan['launch_url']='https://uri.amap.com/navigation?'+urlencode(params)
            plan['link_kind']='navigation'
        else:
            plan['launch_url']='https://uri.amap.com/search?'+urlencode({**params,'keyword':args['destination'],'city':args.get('city','')})
            plan['link_kind']='destination_search'
    return plan


def plan(state,args):
    if args['action']=='query':
        items=state['gui']['plans']
        if args.get('id'):
            items=[p for p in items if p['id']==args['id']]
        return result('已读取操作计划。',{'plans':items,'device_connected':False})
    operations=args.get('operations') or [{k:v for k,v in args.items() if k!='operations'}]
    if not operations or len(operations)>30:
        return result('每个任务需要1至30项操作。',status='needs_input')
    try:
        plans=[build({**args,**operation}) for operation in operations]
    except (ValueError,TypeError) as exc:
        return result(str(exc),status='needs_input')
    combined=plans[0] if len(plans)==1 else {'id':'plan-'+uuid.uuid4().hex,'operation':'sequence',
        'steps':[step for p in plans for step in p['steps']], 'operations':plans,'execution':'planned',
        'device_connected':False,'route_calculated':False,'unresolved_fields':[f for p in plans for f in p['unresolved_fields']]}
    if args['action']=='execute':
        return result('已生成计划；尚未连接能够执行这些应用任务的设备。',{'taskPlan':combined},status='unsupported',execution='not_executed',missing_capabilities=['connected_phone_and_automation_adapter'])
    state['gui']['plans'].append(combined)
    return result('已生成保留具体参数与检查步骤的操作计划。',{'taskPlan':combined},operation_type='300001',execution='planned')
