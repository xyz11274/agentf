"""Real operations for the example agent. Unconnected integrations fail explicitly."""
from __future__ import annotations

from contextlib import contextmanager
from datetime import datetime
import base64
import fcntl
import hashlib
import json
import os
from pathlib import Path
import tempfile
import uuid
from urllib.parse import urlencode, urlparse
from urllib.request import Request, urlopen

from .live_information import result, search, weather, current_time, fetch_json


def save_artifact(store, scope, name, content, media_type):
    root = store.path.parent / 'artifacts' / scope
    root.mkdir(parents=True, exist_ok=True)
    destination = root / Path(name).name
    with tempfile.NamedTemporaryFile(dir=root, delete=False) as stream:
        stream.write(content); temporary = Path(stream.name)
    temporary.replace(destination)
    return {'path': str(destination.resolve()), 'media_type': media_type, 'bytes': len(content),
            'sha256': hashlib.sha256(content).hexdigest()}


def capabilities():
    from .readiness import report
    return report()


def unavailable(capability, missing, data=None):
    return result(f'尚未连接{capability}，没有执行该操作。', data, status='unsupported',
                  missing_capabilities=missing, execution='not_executed')


def navigation(state, args):
    from .gui_plans import plan
    return plan(state,args)


def document(state, args, skill, store, scope):
    bucket = state['documents'].setdefault(skill, {})
    key = args.get('key') or args.get('id')
    if args['action'] in ('query','get_document'):
        docs = [bucket[key]] if key in bucket else list(bucket.values()) if not key else []
        return result('已读取保存的文档。', {'documents':docs}, status='succeeded' if docs else 'not_found')
    if args['action'] == 'delete':
        if not key or key not in bucket:
            return result('文档不存在。', status='not_found')
        del bucket[key]
        return result('已删除文档索引；历史产物文件保留以供审计。')
    if args['action'] == 'calendar_create':
        from .office_files import calendar
        missing = [k for k in ('title','start_time','end_time') if not args.get(k)]
        if missing:
            return result('日程缺少起止时间或标题。',status='needs_input',missing_fields=missing)
        key = key or 'event-'+uuid.uuid4().hex
        artifact = save_artifact(store,scope,hashlib.sha256(key.encode()).hexdigest()+'.ics',calendar(args,key),'text/calendar')
        item = {**args,'key':key,'artifact':artifact,'execution':'calendar_file_saved','invitation_sent':False}
        bucket[key]=item
        return result('已生成可导入的日历文件；尚未向参会人发送邀请。',{'document':item},files=[artifact])
    if not (args.get('content') or args.get('sheets') or args.get('slides')):
        return result('请提供或生成需要保存的正文或表格。', status='needs_input', missing_fields=['content_or_sheets_or_slides'])
    key = key or 'document-'+uuid.uuid4().hex
    previous = bucket.get(key, {})
    title = args.get('title') or previous.get('title',key)
    content = args.get('content') or json.dumps(args.get('sheets') or args.get('slides'),ensure_ascii=False,indent=2)
    versions = previous.get('versions', [])+[content]
    from .office_files import render
    data,extension,mime = render(args,title,content)
    artifact = save_artifact(store, scope, hashlib.sha256(key.encode()).hexdigest()+f'-v{len(versions)}.'+extension, data, mime)
    item = {'key':key, 'title':title, 'content':content,
            'version':len(versions), 'versions':versions, 'artifact':artifact, 'execution':'file_saved'}
    bucket[key] = item
    return result('文档已实际保存。', {'document':item}, files=[artifact])


def image_generate(args, store, scope):
    prompt = args.get('content') or args.get('query')
    if not prompt:
        return result('请给出图片描述。', status='needs_input', missing_fields=['content'])
    key = os.environ.get('OPENAI_IMAGE_API_KEY')
    if not key:
        return unavailable('图片模型', ['OPENAI_IMAGE_API_KEY'])
    base = os.environ.get('OPENAI_IMAGE_BASE_URL', 'https://ark.cn-beijing.volces.com/api/plan/v3').rstrip('/')
    if urlparse(base).scheme != 'https' or urlparse(base).hostname != 'ark.cn-beijing.volces.com':
        raise ValueError('This adapter expects the configured Volcengine HTTPS endpoint')
    payload = {'model':os.environ.get('OPENAI_IMAGE_MODEL','doubao-seedream-5.0-lite'), 'prompt':prompt,
               'size':args.get('size','2K'), 'response_format':'b64_json', 'watermark':True,
               'sequential_image_generation':'disabled'}
    reply = fetch_json(base+'/images/generations', payload=payload, headers={'Authorization':'Bearer '+key}, timeout=120)
    images = reply.get('data', [])
    if not images or not images[0].get('b64_json'):
        return result('图片接口未返回可保存的图片数据。', status='failed', source='Volcengine')
    content = base64.b64decode(images[0]['b64_json'], validate=True)
    if content.startswith(b'\x89PNG\r\n\x1a\n'):
        suffix, mime = '.png', 'image/png'
    elif content.startswith(b'\xff\xd8\xff'):
        suffix, mime = '.jpg', 'image/jpeg'
    elif content[:4] == b'RIFF' and content[8:12] == b'WEBP':
        suffix, mime = '.webp', 'image/webp'
    else:
        raise ValueError('provider output is not a supported image')
    artifact = save_artifact(store, scope, 'image-'+uuid.uuid4().hex+suffix, content, mime)
    return result('图片已由模型生成并保存为实际文件。', {'media_generated':True, 'files':[artifact], 'model':payload['model']},
                  source='Volcengine', files=[artifact])


@contextmanager
def request_lock(store, scope, args):
    # Serialize explicit write retries across skill subprocesses without holding SQLite during I/O.
    from .task_context import current_task
    identity = scope+':'+current_task()+':'+args.get('request_id',uuid.uuid4().hex)
    root = store.path.parent/'locks'; root.mkdir(parents=True, exist_ok=True)
    with (root/(hashlib.sha256(identity.encode()).hexdigest()+'.lock')).open('a') as stream:
        fcntl.flock(stream, fcntl.LOCK_EX)
        yield


def execute(skill, args, store, scope):
    from . import reminders
    from .task_context import receipt_key
    action = args['action']
    if action.startswith('browser_') and args.get('ref') and not args.get('request_id'):
        args = {**args,'request_id':'browser-'+hashlib.sha256(json.dumps(args,sort_keys=True,ensure_ascii=False).encode()).hexdigest()}
    if skill == 'custom-workflow' and action in ('workflow_save','workflow_run'):
        from .workflows import execute as workflow
        with request_lock(store,scope,args):
            return workflow(store,scope,args)
    if skill == 'health-advice' and action.startswith('appointment_'):
        from . import appointments
        appointments.initialize(store)
    with request_lock(store, scope, args):
        if args.get('request_id'):
            with store.connect() as db:
                old = db.execute('SELECT fingerprint,result FROM requests WHERE scope=? AND skill=? AND request_id=?', (scope,receipt_key(skill),args['request_id'])).fetchone()
            if old:
                fingerprint = hashlib.sha256(json.dumps(args,sort_keys=True,ensure_ascii=False).encode()).hexdigest()
                if old[0] != fingerprint:
                    raise ValueError('request_id was already used with different arguments')
                return {**json.loads(old[1]), 'replayed':True}
        prepared = None
        imported = None
        if store.read(scope)['services'].get(skill, True) is False:
            return store.run(scope,skill,args,lambda _state:result('业务能力暂不可用。',status='failed',retryable=True))
        evaluation = store.read(scope).get('evaluation_world')
        if evaluation:
            # 快照世界所有外部读取/写入由下方统一路由，不能漏回实时接口。
            if skill == 'health-advice' and action == 'notifications':
                prepared = result('仿真时钟下的通知。', {'notifications': reminders.notifications(store, scope)})
        elif action.startswith('browser_'):
            from .browser_receipts import execute as browse
            prepared = browse(store,scope,skill,args)
        elif skill == 'smart-home' and args.get('execution_target') == 'home_assistant':
            from .device_connectors import home_assistant
            if action == 'control' and not args.get('request_id'):
                prepared = result('真实家居控制需要 request_id，以便安全重试。',status='needs_input',missing_fields=['request_id'])
            else:
                prepared = home_assistant(args)
        elif skill == 'device-gui' and action.startswith('device_'):
            from .device_connectors import adb_operate
            if action != 'device_observe' and not args.get('request_id'):
                prepared = result('真实设备写操作需要 request_id，以便安全重试。',status='needs_input',missing_fields=['request_id'])
            else:
                prepared = adb_operate(store.read(scope),args,store,scope)
        elif skill == 'web-search' and action == 'current_time':
            prepared = current_time(args)
        elif skill == 'web-search' and action in ('geocode','route'):
            from .map_services import geocode, route
            prepared = geocode(args) if action == 'geocode' else route(args)
        elif skill == 'office-work' and action == 'email_send':
            from .external_connectors import send_email
            prepared = send_email(args)
        elif skill == 'third-party' and action == 'webhook_post':
            from .external_connectors import post_webhook
            prepared = post_webhook(args)
        elif (skill == 'web-search' and action == 'weather') or (skill == 'web-search' and action == 'query' and args.get('topic') == 'weather'):
            prepared = weather(args)
        elif action in ('query','search','recommend') and skill in ('web-search','knowledge-base','third-party','travel-planner','foodie','shopping','movie-query'):
            # movie query without a query is a local playback/list status request.
            if skill != 'movie-query' or action == 'recommend':
                if skill == 'movie-query':
                    preference = store.read(scope)['preferences'].get('电影偏好','')
                    query = args.get('query') or ' '.join(filter(None,[args.get('genre') or preference,args.get('actor'),'电影 推荐']))
                    prepared = search({**args,'query':query})
                else:
                    prepared = search(args)
        elif skill == 'knowledge-base' and action == 'import_url':
            from .knowledge import import_page
            imported = import_page(store,scope,args)
        elif skill == 'movie-query' and action in ('play','pause','resume'):
            from .movies import playback
            from .task_context import task_context
            # 播放器是会话内明确共享的业务资源：新一轮暂停仍操作原播放器。
            # 普通 browser_* 页面按分支隔离；播放器整段操作按资源串行。
            with task_context('movie-player'):
                with request_lock(store, scope, {'request_id': 'player-control'}):
                    prepared = playback(store,scope,args)
        elif skill == 'multimodal' and action == 'generate_image':
            prepared = image_generate(args,store,scope)
        elif skill == 'health-advice' and action == 'notifications':
            prepared = result('本服务实际到时事件。',{'notifications':reminders.notifications(store,scope), 'scheduler_running':reminders.running(store)})
        scheduler_running = reminders.running(store) if skill == 'health-advice' and action.startswith('reminder_') else False

        def run(state, db):
            from .evaluation_world import execute as evaluate_action, clock
            answer = evaluate_action(state, skill, args)
            if answer is not None:
                pass
            elif prepared is not None:
                if prepared['status'] == 'succeeded':
                    state.setdefault('last_information',{})[skill] = prepared['UserOutputObject']
                    if skill in ('device-gui','multi-device') and prepared['UserOutputObject'].get('device_id'):
                        state['selected_external_device'] = prepared['UserOutputObject']['device_id']
                answer = prepared
            elif skill == 'knowledge-base':
                from .knowledge import execute as knowledge
                answer = knowledge(state,args,imported)
            elif skill == 'user-memory':
                from .services import memory
                answer = {**memory(state,args),'simulated':False,'source':'local-sqlite'}
            elif skill == 'device-gui':
                answer = navigation(state,args)
            elif skill == 'health-advice':
                if action.startswith('appointment_'):
                    answer = appointments.execute(state,args,db,store.owner_scope(scope,db),now=clock(state))
                elif action == 'reminder_create':
                    answer = reminders.create(state,args,store,scope)
                elif action.startswith('reminder_'):
                    answer = reminders.manage(state,args,store,scope)
                elif action.startswith('record_'):
                    from . import health_records
                    if action == 'record_save':
                        answer = health_records.save(state,args)
                    elif action == 'record_delete':
                        answer = health_records.delete(state,args)
                    else:
                        answer = health_records.query(state,args)
                elif action == 'triage':
                    answer = result('已读取用户提供的病史；由健康 skill 结合具体症状提供一般建议。',
                                    {'symptoms':args.get('query',''),'records':[r for r in state['records'].values() if not r.get('deleted_at') and (not args.get('patient') or r.get('patient')==args['patient'])],'diagnosis':None},operation_type='100155')
                else:
                    answer = unavailable('医院预约系统',['hospital_booking_provider','patient_account'])
                answer.setdefault('UserOutputObject',{})['scheduler_running'] = scheduler_running
            elif skill == 'smart-home':
                from .device_world import execute as execute_home
                answer = execute_home(state,args)
            elif skill == 'multi-device':
                from .device_connectors import connected_devices
                answer = connected_devices(state,args)
            elif skill == 'service-manager' or (skill == 'third-party' and action == 'status'):
                answer = result('当前实际能力与缺口。',{'capabilities':capabilities()})
            elif skill == 'movie-query':
                from .movies import execute as movie
                answer = movie(state,args)
            elif skill == 'multimodal' and action == 'generate_video':
                answer = unavailable('可用的视频模型',['video_model_access_currently_UnsupportedModel'])
            else:
                answer = document(state,args,skill,store,scope)
            if evaluation:
                answer = {**answer, 'simulated': True, 'snapshot_sha256': evaluation['sha256'],
                          'environment': 'business_test_system', 'physical_effect': False}
            return {**answer,'skill':skill,'execution_mode':'real'}
        return store.run(scope,skill,args,run,transactional=True)
