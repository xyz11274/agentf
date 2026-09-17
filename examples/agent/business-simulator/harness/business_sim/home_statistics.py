"""Time-bucketed telemetry from the running test home, never invented history."""
from datetime import datetime,timedelta,time as daytime
from zoneinfo import ZoneInfo
import statistics
from .time_constraints import dates,clock_constraints,matches_clock

ZONE=ZoneInfo('Asia/Shanghai')

def slices(start,end):
    while start<end:
        stamp=datetime.fromtimestamp(start,ZONE)
        boundary=(stamp.replace(minute=0,second=0,microsecond=0)+timedelta(hours=1)).timestamp()
        stop=min(boundary,end)
        yield stamp.strftime('%Y-%m-%dT%H:00'),stop-start,start,stop
        start=stop

def energy(state,device,start,seconds):
    if seconds<=0:return
    for hour,duration,left,right in slices(start,start+seconds):
        key=device['id']+'/'+hour
        row=state.setdefault('home_energy',{}).setdefault(key,{'device_id':device['id'],'hour':hour,
            'powered_seconds':0.,'energy_kwh':0.,'start':left,'end':right})
        row['powered_seconds']+=duration
        row['energy_kwh']+=device['watts']*duration/3600000
        row['end']=right

def concurrent(state,start,end):
    powered=sorted(d['id'] for d in state['devices'] if d['online'] and d['power'] and d['kind']!='curtain')
    for i,first in enumerate(powered):
        for second in powered[i+1:]:
            for hour,duration,left,right in slices(start,end):
                key=first+'/'+second+'/'+hour
                row=state.setdefault('home_intervals',{}).setdefault(key,{'devices':[first,second],'hour':hour,
                    'seconds':0.,'start':left,'end':right})
                row['seconds']+=duration;row['end']=right

def analyze(state,args,selected):
    today=datetime.now(ZONE).date()
    period=args.get('date','')
    if args.get('start_date'):
        start=datetime.fromisoformat(args['start_date']).date()
        end=datetime.fromisoformat(args.get('end_date',args['start_date'])).date()
    else:
        window=dates(period,today)
        if period and window is None:raise ValueError('请明确查询起止日期')
        start,end=window if window else (None,None)
    if start and end<start:raise ValueError('结束日期不能早于开始日期')
    clock=clock_constraints({'time_period':args.get('time_period','')})
    def include(day,hm):
        return (start is None or start.isoformat()<=day<=end.isoformat()) and matches_clock(hm,clock)
    records=[r for r in state.get('home_energy',{}).values() if r['device_id'] in selected and include(r['hour'][:10],r['hour'][11:16])]
    history=[]
    for h in state['home_history']:
        stamp=datetime.fromtimestamp(h['timestamp'],ZONE)
        if h['device_id'] in selected and include(stamp.strftime('%Y-%m-%d'),stamp.strftime('%H:%M')):
            history.append(h)
    totals={}
    for r in records:
        row=totals.setdefault(r['device_id'],{'device_id':r['device_id'],'energy_kwh':0.,'powered_seconds':0.})
        row['energy_kwh']+=r['energy_kwh'];row['powered_seconds']+=r['powered_seconds']
    for row in totals.values():
        row['energy_kwh']=round(row['energy_kwh'],8);row['powered_hours']=round(row.pop('powered_seconds')/3600,4)
    modes=[]
    for verb,mode in [('on','ARRIVE_HOME_MODE'),('off','LEAVE_HOME_MODE')]:
        samples=[h for h in history if h['command']['action']==verb]
        days={datetime.fromtimestamp(h['timestamp'],ZONE).date() for h in samples}
        if len(days)<3:continue
        stamps=[datetime.fromtimestamp(h['timestamp'],ZONE) for h in samples]
        minute=int(statistics.median(t.hour*60+t.minute for t in stamps))
        modes.append({'mode':mode,'habitTime':f'{minute//60:02d}:{minute%60:02d}','observation_days':len(days),
                      'commandList':[{'commandText':h['text']} for h in samples[-3:]]})
    pairs={}
    compare=args.get('compare_device')
    for row in state.get('home_intervals',{}).values():
        if not any(d in selected for d in row['devices']) or not include(row['hour'][:10],row['hour'][11:16]):continue
        if compare and compare not in row['devices']:continue
        key=tuple(row['devices']);pairs[key]=pairs.get(key,0)+row['seconds']
    return {'analysis':args.get('analysis','habits'),'start_date':start.isoformat() if start else None,
            'end_date':end.isoformat() if end else None,'timezone':'Asia/Shanghai','modeList':modes,
            'history':history[-100:],'sample_count':len(history),'enough_history':bool(modes),
            'energy':list(totals.values()),'hourly_usage':records,
            'cooccurrence':[{'devices':list(k),'simultaneous_hours':round(v/3600,4)} for k,v in pairs.items()],
            'telemetry_hours':len({r['hour'] for r in records}),
            'energy_basis':'test_device_nominal_watts_times_observed_runtime; not physical meter readings',
            'inference':'observed switch time median; not presence detection',
            'coverage_note':'只统计后端实际运行期间积累的记录；空历史不代表零用电或无习惯'}

