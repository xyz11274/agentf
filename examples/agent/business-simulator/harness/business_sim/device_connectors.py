"""Optional real-device adapters. Credentials and device output never enter fixtures."""
from __future__ import annotations
import base64
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import xml.etree.ElementTree as ET
from urllib.parse import urlsplit
from urllib.request import Request, urlopen
from urllib.error import HTTPError, URLError
from .live_information import result

def home_assistant_ready():
    return bool(os.environ.get('HOME_ASSISTANT_URL') and os.environ.get('HOME_ASSISTANT_TOKEN'))

def _ha(path, payload=None, timeout=25):
    base=os.environ.get('HOME_ASSISTANT_URL','').rstrip('/')
    token=os.environ.get('HOME_ASSISTANT_TOKEN','')
    parsed=urlsplit(base)
    allow_http=os.environ.get('HOME_ASSISTANT_ALLOW_HTTP')=='1'
    if not token or parsed.scheme not in (('https','http') if allow_http else ('https',)) or not parsed.hostname or parsed.username or parsed.password:
        raise ValueError('HOME_ASSISTANT_URL/TOKEN not configured; HTTPS is required unless operator enables HTTP')
    body=json.dumps(payload).encode() if payload is not None else None
    req=Request(base+path,data=body,headers={'Authorization':'Bearer '+token,'Content-Type':'application/json'},method='POST' if body else 'GET')
    try:
        with urlopen(req,timeout=timeout) as reply:return json.load(reply)
    except HTTPError as exc:
        return {'_error':f'HTTP {exc.code}'}
    except (URLError, OSError, TimeoutError):
        return {'_error':'connection_failed'}

def home_assistant_connected():
    if not home_assistant_ready():
        return False
    try:
        states=_ha('/api/states',timeout=5)
    except (TypeError, ValueError):
        return False
    return isinstance(states,list)


def _ha_entities(args):
    entities=_ha('/api/states')
    if isinstance(entities,dict) and entities.get('_error'):
        return entities
    room=(args.get('room') or '').lower();name=(args.get('device') or '').lower();identity=args.get('device_id')
    rows=[]
    for item in entities:
        entity=item.get('entity_id','')
        friendly=str(item.get('attributes',{}).get('friendly_name',''))
        text=(entity+' '+friendly).lower()
        if identity and identity!=entity:continue
        if name and name not in text:continue
        if room and room not in text:continue
        rows.append(item)
    return rows

def home_assistant(args):
    if not home_assistant_ready():
        return result('Home Assistant 未配置。',status='unsupported',
                      missing_capabilities=['HOME_ASSISTANT_URL','HOME_ASSISTANT_TOKEN'],execution='not_executed')
    if args['action']=='query':
        entities=_ha_entities(args)
        if isinstance(entities,dict):
            return result('Home Assistant 查询失败。',entities,status='failed',execution='not_executed')
        return result('已从 Home Assistant 读取实时实体状态。',{'devices':entities,'connected':True},
                      source='home-assistant',execution='observed',environment='physical_home',
                      physical_effect=False)
    commands=args.get('commands',[])
    if not commands:
        return result('需要实际控制命令。',status='needs_input',missing_fields=['commands'])
    receipts=[]
    for command in commands:
        entity=command.get('device_id')
        if not entity or '.' not in entity:
            return result('Home Assistant 控制必须使用 query 返回的 entity_id。',
                          status='needs_input',missing_fields=['commands[].device_id'])
        domain=entity.split('.',1)[0];action=command['action'];prop=command.get('property');value=command.get('value')
        service=None;payload={'entity_id':entity}
        if action in ('on','off') and domain in ('light','switch','fan','climate','media_player','cover'):
            service='turn_'+action
        elif action=='set':
            mapping={
              ('climate','temperature'):('set_temperature','temperature'),
              ('climate','mode'):('set_hvac_mode','hvac_mode'),
              ('light','brightness'):('turn_on','brightness_pct'),
              ('light','color'):('turn_on','color_name'),
              ('cover','position'):('set_cover_position','position'),
              ('media_player','volume'):('volume_set','volume_level'),
              ('fan','fan_speed'):('set_percentage','percentage'),
            }
            selected=mapping.get((domain,prop))
            if selected:service,field=selected;payload[field]=value/100 if field=='volume_level' else value
        if not service:
            return result(f'不支持 Home Assistant 实体 {entity} 的该属性。',
                          status='unsupported',execution='not_executed')
        answer=_ha(f'/api/services/{domain}/{service}',payload)
        if isinstance(answer,dict) and answer.get('_error'):
            return result('Home Assistant 拒绝控制命令。',{'receipt':answer},status='failed',
                          source='home-assistant',execution='not_verified')
        receipts.append({'entity_id':entity,'service':f'{domain}.{service}','accepted':True})
    observed=_ha_entities({})
    by_id={item['entity_id']:item for item in observed} if isinstance(observed,list) else {}
    return result('Home Assistant 已接受命令并回读实体状态。',
                  {'receipts':receipts,'devices':[by_id[r['entity_id']] for r in receipts if r['entity_id'] in by_id],
                   'command_count':len(receipts)},source='home-assistant',execution='executed_and_state_observed',
                  environment='physical_home',physical_effect='state_observed_not_physical_sensor_verified')

def _adb_bin():
    return os.environ.get('BUSINESS_ADB_EXECUTABLE') or shutil.which('adb')

def adb_devices():
    binary=_adb_bin()
    if not binary:return []
    try:
        done=subprocess.run([binary,'devices','-l'],capture_output=True,text=True,timeout=10,check=False)
    except (OSError, subprocess.TimeoutExpired):
        return []
    if done.returncode:
        return []
    devices=[]
    for line in done.stdout.splitlines()[1:]:
        parts=line.split()
        if len(parts)>=2:
            attrs=dict(piece.split(':',1) for piece in parts[2:] if ':' in piece)
            devices.append({'id':parts[0],'status':parts[1],'model':attrs.get('model',''),
                            'product':attrs.get('product',''),'transport':'adb'})
    return devices

def _serial(args,state):
    requested=args.get('device_id') or state.get('selected_external_device')
    online=[d for d in adb_devices() if d['status']=='device']
    if requested and any(d['id']==requested for d in online):return requested
    return online[0]['id'] if len(online)==1 else None

def _adb(serial,*args,timeout=30,binary=True):
    done=subprocess.run([_adb_bin(),'-s',serial,*args],capture_output=True,timeout=timeout,check=False)
    if done.returncode:
        return None
    return done.stdout if binary else done.stdout.decode(errors='replace')

BOUNDS=re.compile(r'\[(\d+),(\d+)\]\[(\d+),(\d+)\]')

def _hierarchy(serial):
    raw=_adb(serial,'exec-out','uiautomator','dump','/dev/tty')
    if not raw:return []
    start=raw.find(b'<?xml')
    if start<0:return []
    try:root=ET.fromstring(raw[start:])
    except ET.ParseError:return []
    elements=[]
    for index,node in enumerate(root.iter('node')):
        attrs=node.attrib
        match=BOUNDS.fullmatch(attrs.get('bounds',''))
        if not match:continue
        x1,y1,x2,y2=map(int,match.groups())
        password=attrs.get('password')=='true'
        if x2<=x1 or y2<=y1:continue
        elements.append({'ref':f'adb-{index}','text':'' if password else attrs.get('text',''),
          'description':attrs.get('content-desc',''),'resource_id':attrs.get('resource-id',''),
          'class':attrs.get('class',''),'clickable':attrs.get('clickable')=='true',
          'editable':attrs.get('class','').endswith(('EditText','AutoCompleteTextView')),
          'bounds':[x1,y1,x2,y2],'center':[(x1+x2)//2,(y1+y2)//2],
          'password':password,'package':attrs.get('package','')})
    return elements

def _screen(serial,store,scope):
    png=_adb(serial,'exec-out','screencap','-p')
    if not png or not png.startswith(b'\x89PNG'):return None
    root=store.path.parent/'artifacts'/str(scope);root.mkdir(parents=True,exist_ok=True)
    path=root/('android-'+datetime.now(timezone.utc).strftime('%Y%m%d-%H%M%S-%f')+'.png')
    path.write_bytes(png)
    return {'path':str(path.resolve()),'bytes':len(png)}

def adb_operate(state,args,store,scope):
    serial=_serial(args,state)
    if not serial:
        return result('没有唯一且在线的 Android ADB 设备。',{'devices':adb_devices()},
                      status='unsupported',missing_capabilities=['authorized_adb_device'],
                      execution='not_executed')
    action=args['action'];elements=_hierarchy(serial);chosen=None
    if args.get('ref'):
        chosen=next((e for e in elements if e['ref']==args['ref']),None)
        if not chosen:return result('界面元素引用已过期，请重新观察。',status='needs_input',missing_fields=['ref'])
    if action=='device_tap':
        if chosen:x,y=chosen['center']
        else:x,y=args.get('x'),args.get('y')
        if x is None or y is None:return result('需要观察返回的 ref 或明确坐标。',status='needs_input')
        if _adb(serial,'shell','input','tap',str(x),str(y)) is None:
            return result('ADB 点击失败，未确认操作。',status='failed',execution='not_verified')
    elif action=='device_swipe':
        values=[args.get(k) for k in ('x','y','x2','y2')]
        if any(v is None for v in values):return result('滑动需要起止坐标。',status='needs_input')
        if _adb(serial,'shell','input','swipe',*[str(v) for v in values],str(args.get('duration_ms',300))) is None:
            return result('ADB 滑动失败，未确认操作。',status='failed',execution='not_verified')
    elif action=='device_type':
        text=args.get('text')
        if text is None:return result('需要输入文字。',status='needs_input',missing_fields=['text'])
        if chosen:
            x,y=chosen['center']
            if _adb(serial,'shell','input','tap',str(x),str(y)) is None:
                return result('ADB 聚焦输入框失败。',status='failed',execution='not_verified')
        encoded=base64.b64encode(text.encode()).decode()
        # am broadcast avoids shell interpolation; receiver must be operator-installed.
        receiver=os.environ.get('BUSINESS_ADB_TEXT_RECEIVER')
        if receiver:
            if _adb(serial,'shell','am','broadcast','-a',receiver,'--es','text_b64',encoded) is None:
                return result('ADB 文本接收器未确认输入。',status='failed',execution='not_verified')
        elif re.fullmatch(r'[A-Za-z0-9 @._+-]*',text):
            if _adb(serial,'shell','input','text',text.replace(' ','%s')) is None:
                return result('ADB 文字输入失败。',status='failed',execution='not_verified')
        else:
            return result('中文输入需要配置可信 ADB 文本输入广播接收器。',
                          status='unsupported',missing_capabilities=['BUSINESS_ADB_TEXT_RECEIVER'])
    elif action=='device_key':
        key=args.get('keycode')
        if not key or not re.fullmatch(r'(?:KEYCODE_)?[A-Z0-9_]+',key):
            return result('需要合法 Android keycode。',status='needs_input',missing_fields=['keycode'])
        if _adb(serial,'shell','input','keyevent',key) is None:
            return result('ADB 按键失败。',status='failed',execution='not_verified')
    elif action=='device_open_app':
        package=args.get('package')
        if not package or not re.fullmatch(r'[A-Za-z][A-Za-z0-9_.]+',package):
            return result('需要 Android package 名。',status='needs_input',missing_fields=['package'])
        if _adb(serial,'shell','monkey','-p',package,'-c','android.intent.category.LAUNCHER','1') is None:
            return result('ADB 应用启动失败。',status='failed',execution='not_verified')
    elif action!='device_observe':
        return result('不支持的真实设备动作。',status='unsupported')
    new_elements=_hierarchy(serial);screen=_screen(serial,store,scope)
    state['selected_external_device']=serial
    return result('已在连接的 Android 设备执行并重新观察界面。',
                  {'device_id':serial,'elements':new_elements,'screenshot':screen,
                   'observation_is_real':True,'business_outcome_verified':False},
                  source='android-adb',execution='executed' if action!='device_observe' else 'observed',
                  files=[screen] if screen else [])



def physical_devices():
    devices=adb_devices()
    if home_assistant_connected():
        devices.append({'id':'home-assistant','status':'connected','transport':'home-assistant','model':'reachable server'})
    for device in devices:
        device.update(device_kind='physical',simulated=False,physical_effect='adapter_dependent')
    return devices


def connected_devices(state,args,physical=None):
    from .virtual_devices import ensure, execute as execute_virtual, public_device
    devices=physical_devices() if physical is None else physical
    world=ensure(state)
    virtual=world['devices']
    target=args.get('execution_target','all')
    if args['action']=='list':
        visible=(devices if target in ('all','physical') else [])+([public_device(item) for item in virtual] if target in ('all','virtual') else [])
        selected=world.get('selected') or state.get('selected_external_device')
        text='已读取真实连接与虚拟测试设备。' if target=='all' else ('已读取实际设备连接。' if target=='physical' else '已读取虚拟测试设备群。')
        if not visible:
            return result('当前没有已授权实体设备连接。',{'devices':[],'selected':selected},status='unsupported',execution='not_executed')
        answer=result(text,{'devices':visible,'selected':selected,
                            'physical_count':len(devices),'virtual_count':len(virtual) if target in ('all','virtual') else 0},execution='observed')
        if not devices and target=='all':
            answer.update(simulated=True,environment='business_test_system',physical_effect=False)
        return answer
    if args['action'] not in ('select',):
        if target=='physical':
            return result('该动作只由虚拟多设备测试总线提供。',status='unsupported',execution='not_executed')
        return execute_virtual(state,args)
    wanted=args.get('device_id')
    if not wanted:return result('请选择 list 返回的设备 id。',status='needs_input',missing_fields=['device_id'])
    if any(d['id']==wanted for d in virtual):
        return execute_virtual(state,{**args,'action':'select'})
    if not any(d['id']==wanted and d['status'] in ('device','connected') for d in devices):
        return result('设备未连接或未授权。',{'devices':devices},status='not_found',execution='not_executed')
    state['selected_external_device']=wanted
    return result('已选择真实设备，后续操作仍需回读设备结果。',{'device_id':wanted,'devices':devices},execution='selected')
