#!/usr/bin/env python3
"""Read-only validation of existing experiment artifacts; never executes models or rescoring."""
import collections
import json
import math
from pathlib import Path
import statistics
import subprocess

ROOT = Path(__file__).resolve().parents[2]
AW = ROOT / 'trace_output/appworld'
DATA = ROOT / 'tools/trajectory_evolution/experiments/appworld/datasets'
checks = []
def read(path): return json.loads(Path(path).read_text())
def lines(path): return [json.loads(x) for x in Path(path).read_text().splitlines() if x.strip()]
def check(name, condition, detail):
    if not condition: raise AssertionError(name + ': ' + str(detail))
    checks.append({'name': name, 'status': 'pass', 'detail': detail})
def close(a,b): return math.isclose(a,b,rel_tol=1e-12,abs_tol=1e-12)
def summed(rows,field): return sum(r[field] for r in rows)
def known(rows,field): return [r[field] for r in rows if isinstance(r.get(field),(int,float))]

b=read(AW/'gepa/l0-current-stage500/result.json')
c=read(AW/'gepa/seed-search-71-stage500/comparison.json')
m=read(AW/'published/strategy/manifest.json')
t=read(AW/'gepa/workspace/agent-evaluation/f0d143a0298773c1bb0b0fbe1733f68feab42c2e53cf49b33e005c1d46f193b2/search/tree.json')
br=b['items'];cr=c['items'];bm={r['task_id']:r for r in br};cm={r['task_id']:r for r in cr}
head=subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip()
check('01 Current checkout identity',head=='f44bbaf4a9531731bfbd3eb6c115fe4210df95b1',head)
dsets={k:lines(DATA/f'{k}.jsonl') for k in ['train','val','holdout','comparison-500']}
counts={k:len(v) for k,v in dsets.items()}
check('02 AppWorld input counts',counts=={'train':134,'val':12,'holdout':500,'comparison-500':500},counts)
views=collections.Counter(r['attributes']['appworld']['source_view'] for r in dsets['comparison-500'])
check('03 Development500 source views',dict(views)=={'test_normal':131,'test_challenge':327,'dev':42},dict(views))
ids=lambda rows:[r['task_id'] for r in rows]
check('04 Same500 exact task order',ids(br)==ids(cr)==ids(dsets['holdout'])==ids(dsets['comparison-500']),{'baseline':len(br),'candidate':len(cr)})
check('05 Unique500 task identities',len(br)==len(cr)==len(bm)==len(cm)==500,'500 unique per side')
check('06 Complete score summary rows',all(r['execution_status']=='completed' and r['quality_status']=='complete' and r.get('error_code') is None for r in br+cr),'1000 summary rows complete; original raw evidence unavailable')
strict=[summed(rs,'strict_passed') for rs in [br,cr]]
check('07 Formal strict totals',strict==[322,338] and strict==[b['metrics']['strict_passed'],c['metrics']['seed_candidate_typeerror_causal_current']['strict_passed']],strict)
q=[statistics.mean(r['quality'] for r in rs) for rs in [br,cr]]
check('08 Formal single-goal quality',q==[.644,.676] and all(r['quality']==r['strict_passed'] for r in br+cr),q)
loops=[summed(rs,'invalid_loop_count') for rs in [br,cr]]
check('09 Complete invalid-loop totals',loops==[190,102] and [x/500 for x in loops]==[.38,.204],loops)
wlt=collections.Counter('win' if cm[k]['strict_passed']>bm[k]['strict_passed'] else 'loss' if cm[k]['strict_passed']<bm[k]['strict_passed'] else 'tie' for k in bm)
check('10 Paired strict wins-losses-ties',dict(wlt)=={'win':78,'loss':62,'tie':360} and wlt['win']-wlt['loss']==16,dict(wlt))
gain=(338-322)/322;reduction=(190-102)/190;needed=math.ceil(322*1.05)
check('11 Exact goal math',close(gain,.04968944099378882) and gain<.05 and needed==339 and close(reduction,.4631578947368421) and reduction>=.15,{'relative_quality_gain':gain,'loop_reduction':reduction,'strict_needed':needed})
levels={label:{f:summed(rs,f) for f in ['l1_applied','l2_action_applied','l2_schedule_applied','l3_applied']} for label,rs in [('L0',br),('candidate',cr)]}
check('12 Actual level application totals',list(levels['L0'].values())==[0,0,0,0] and list(levels['candidate'].values())==[595,0,0,4],levels)
coverage={label:{f:len(known(rs,f)) for f in ['latency_ms','tokens','model_calls']} for label,rs in [('L0',br),('candidate',cr)]}
means={f:statistics.mean(known(cr,f)) for f in ['latency_ms','tokens','model_calls']}
check('13 Resource coverage and partial means',list(coverage['L0'].values())==[0,0,0] and list(coverage['candidate'].values())==[500,497,497] and close(means['latency_ms'],172433.68371563) and close(means['tokens'],172923.97987927566) and close(means['model_calls'],25.722334004024145),{'coverage':coverage,'candidate_means':means,'all_attempt_costs_unknown':True})
extra={label:[r['task_id'] for r in rs if r['official_quality']==1 and not r['strict_passed']] for label,rs in [('L0',br),('candidate',cr)]}
check('14 Official versus formal strict',b['metrics']['official_strict_passed']==323 and c['metrics']['seed_candidate_typeerror_causal_current']['official_strict_passed']==339 and extra=={'L0':['appworld-9a16324_3'],'candidate':['appworld-3b8fb7a_2']},extra)
check('15 Delivery is not qualified publication',m['status']=='delivered' and m['release_status']=='best_available_not_qualified' and m['goal_passed'] is False and m['gepa_completion_claimed'] is False and m['independent_holdout'] is False,{k:m[k] for k in ['status','release_status','goal_passed','gepa_completion_claimed','independent_holdout']})
check('16 Delivered source equality and seed identity',(AW/'published/strategy/strategy.json').read_bytes()==(AW/'published/candidate-typeerror-causal-current/strategy.json').read_bytes() and read(AW/'published/strategy/comparison.json')==c and m['version']==t['seed_version']==t['nodes'][0]['version'],'Historical seed label71 equals current tree Node0; original run git commit not proven')
status=collections.Counter(n['status'] for n in t['nodes'][1:])
expected={'error':13,'duplicate':5,'untriggerable_controls':4,'interrupted':3,'inconclusive':2,'unobserved_changes':2,'rejected_minibatch':1}
check('17 GEPA30 recorded terminal state',len(t['nodes'])==31 and dict(status)==expected and t['model_calls']==395 and t['status']=='cancelled' and t['active'] is None and t['nodes'][30]['parent']==0 and t['nodes'][30]['screening']['candidate']=={}, {'statuses':dict(status),'optimizer_calls':395,'final_holdout_field':t['final_holdout']['status']})
raw=m['native_source_reports']+[m['baseline_source_report']]
mapped=[ROOT/p.removeprefix('/root/lai-code/Agent/AgentFlow/') for p in raw]
check('18 Missing native provenance files',len(mapped)==7 and all(not p.exists() for p in mapped),{'missing':7,'source_prefix':'/root/lai-code/Agent/AgentFlow/','mapped_prefix':str(ROOT)})
BUS=ROOT/'trace_output/business-simulator/published/seed004'
s=read(BUS/'baseline-cache/comparison300.json');bc=read(BUS/'comparison.json');manifest=read(BUS/'manifest.json')
check('19 Business selection bias and full400 regression',s['method']=='performance_informed_business_acceptance' and s['scope']==300 and s['full400']['L0']['strict_passed']==177 and s['full400']['candidate']['strict_passed']==168 and s['full400']['candidate']['quality']<s['full400']['L0']['quality'] and s['full400']['candidate']['loop']>s['full400']['L0']['loop'] and bc['scope']=='known_business300_plus_training_replay200' and bc['independent_holdout'] is False and manifest['independent_holdout'] is False,{'selected_300_method':s['method'],'full400_strict':[177,168],'final500_independent_holdout':False})
missing=['trace_output/bfcl-v3','trace_output/tau2-retail-verified','tools/trajectory_evolution/experiments/tau2-retail-verified/tau2_host.py','tools/trajectory_evolution/experiments/tau2-retail-verified/datasets/native','tools/trajectory_evolution/experiments/tau2-retail-verified/runtime/venv/bin/python','trace_output/business-simulator/published/seed004/runtime/trajectory_evolution','examples/agent/business-simulator/appworld/runtime/venv/bin/python','examples/agent/business-simulator/appworld/runtime/world','build/agentflowd','tools/trajectory_evolution/build-v4/trajectory_evolution']
check('20 Local benchmark results and runtime missing',all(not (ROOT/p).exists() for p in missing),missing)
assert len(checks)==20
out=Path(__file__).resolve().parent / '实验数据核查回执.json'
out.write_text(json.dumps({'validation':'read-only compact artifact checks; no raw provenance confirmation or experiment rerun','passed':len(checks),'checks':checks},ensure_ascii=False,indent=2)+'\n')
print(f'{len(checks)}/20 artifact assertions PASS; no models, scoring, service, or experiment-data writes')
print(out)
