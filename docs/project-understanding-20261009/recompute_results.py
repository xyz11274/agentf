#!/usr/bin/env python3
"""Read existing compact experiment artifacts and write report-side metrics.

No model execution, rescoring, service launch, memory updates, or modification
of experiment artifacts. Summary consistency is not raw-evidence verification.
"""
from pathlib import Path
from collections import Counter
import json, math, statistics, subprocess

OUT=Path(__file__).resolve().parent
ROOT=OUT.parents[1]
AW=ROOT/'trace_output/appworld'
DATA=ROOT/'tools/trajectory_evolution/experiments/appworld/datasets'
BUS=ROOT/'trace_output/business-simulator/published/seed004'
def read(p):return json.loads(p.read_text())
def rows(p):return [json.loads(line) for line in p.read_text().splitlines() if line.strip()]
def numeric(v):return isinstance(v,(int,float)) and not isinstance(v,bool) and math.isfinite(v)
def summary(rs):
    result={'tasks':len(rs),'completed':sum(r.get('execution_status')=='completed' for r in rs),
            'strict_passed':sum(r['strict_passed'] for r in rs),
            'quality_complete':sum(r.get('quality_status')=='complete' for r in rs),
            'loop_review_status_field':{
                'present_rows':sum('loop_review_status' in r for r in rs),
                'complete_rows':sum(r.get('loop_review_status')=='complete' for r in rs)},
            'error_rows':sum(bool(r.get('error_code')) for r in rs)}
    for f in ['quality','invalid_loop_count','latency_ms','tokens','model_calls',
              'l1_applied','l2_action_applied','l2_schedule_applied','l3_applied']:
        known=[r[f] for r in rs if numeric(r.get(f))]
        result[f]={'coverage':len(known),'total':sum(known) if known else None,
                   'mean':statistics.mean(known) if known else None}
    result['tasks_with_applications']={f:sum(numeric(r.get(f)) and r[f]>0 for r in rs)
                     for f in ['l1_applied','l2_action_applied','l2_schedule_applied','l3_applied']}
    result['policy_loaded_versions']=dict(Counter(r.get('loaded_version') for r in rs))
    return result

b=read(AW/'gepa/l0-current-stage500/result.json')
c=read(AW/'gepa/seed-search-71-stage500/comparison.json')
m=read(AW/'published/strategy/manifest.json')
tpath=AW/'gepa/workspace/agent-evaluation/f0d143a0298773c1bb0b0fbe1733f68feab42c2e53cf49b33e005c1d46f193b2/search/tree.json'
t=read(tpath)
br=b['items'];cr=c['items'];bid=[r['task_id'] for r in br];cid=[r['task_id'] for r in cr]
datasets={k:rows(DATA/f'{k}.jsonl') for k in ['train','val','holdout','comparison-500']}
assert bid==cid==[r['task_id'] for r in datasets['holdout']]==[r['task_id'] for r in datasets['comparison-500']]
assert len(bid)==len(set(bid))==500
taskmeta={r['task_id']:r['attributes']['appworld'] for r in datasets['comparison-500']}
wlt=Counter('win' if r['strict_passed']>s['strict_passed'] else 'loss' if r['strict_passed']<s['strict_passed'] else 'tie' for r,s in zip(cr,br))
subgroups={}
for view in ['test_normal','test_challenge','dev']:
    ix=[i for i,tid in enumerate(bid) if taskmeta[tid]['source_view']==view]
    subgroups[view]={'L0':summary([br[i] for i in ix]),'frozen_seed':summary([cr[i] for i in ix]),
                     'paired':dict(Counter('win' if cr[i]['strict_passed']>br[i]['strict_passed'] else
                                           'loss' if cr[i]['strict_passed']<br[i]['strict_passed'] else 'tie' for i in ix))}
checks=[]
def check(label,condition):
    assert condition,label
    checks.append({'check':label,'status':'pass'})
bs=summary(br);cs=summary(cr)
for summed,aggregate in [(bs,b['metrics']),(cs,c['metrics']['seed_candidate_typeerror_causal_current'])]:
    summed['aggregate_loop_assessment']={
        key:aggregate.get(key) for key in ['invalid_loop_status','invalid_loop_coverage','loop_review_coverage','loop_basis']}
check('AppWorld same-task compact complete500',bs['tasks']==cs['tasks']==500 and bs['completed']==cs['completed']==500)
check('Formal strict totals322/338',bs['strict_passed']==322 and cs['strict_passed']==338)
check('Q and strict coincide for single-goal tasks',all(r['quality']==r['strict_passed'] for r in br+cr))
check('Invalid loop totals190/102',bs['invalid_loop_count']['total']==190 and cs['invalid_loop_count']['total']==102)
check('Pair wins78/losses62/ties360',dict(wlt)=={'win':78,'loss':62,'tie':360})
check('No L2; candidate L1=595/L3=4',cs['l1_applied']['total']==595 and cs['l2_action_applied']['total']==0 and cs['l2_schedule_applied']['total']==0 and cs['l3_applied']['total']==4)
check('Official323/339 is separate from formal322/338',b['metrics']['official_strict_passed']==323 and c['metrics']['seed_candidate_typeerror_causal_current']['official_strict_passed']==339)
check('Candidate resource coverage500/497/497',cs['latency_ms']['coverage']==500 and cs['tokens']['coverage']==cs['model_calls']['coverage']==497)
check('L0 resource values missing',bs['latency_ms']['coverage']==bs['tokens']['coverage']==bs['model_calls']['coverage']==0)
check('View counts131/327/42', [subgroups[v]['L0']['tasks'] for v in subgroups]==[131,327,42])
check('Delivery explicitly unqualified and nonindependent',m['release_status']=='best_available_not_qualified' and m['goal_passed'] is False and m['independent_holdout'] is False)
check('Seed equals current treeNode0',m['version']==t['nodes'][0]['version']==t['seed_version'])
new_status=Counter(n['status'] for n in t['nodes'][1:])
check('GEPA30 new nodes,0accepted,395calls,cancelled',len(t['nodes'])==31 and not new_status['accepted'] and t['model_calls']==395 and t['status']=='cancelled')
raw=m['native_source_reports']+[m['baseline_source_report']]
mapped=[ROOT/p.removeprefix('/root/lai-code/Agent/AgentFlow/') for p in raw]
check('Seven referenced native reports absent locally',len(mapped)==7 and all(not p.exists() for p in mapped))
bc=read(BUS/'comparison.json');b300=read(BUS/'baseline-cache/comparison300.json');bman=read(BUS/'manifest.json')
check('Business300 selected using measured results',b300['method']=='performance_informed_business_acceptance')
check('Business full400 strict regression177to168',b300['full400']['L0']['strict_passed']==177 and b300['full400']['candidate']['strict_passed']==168)
check('Business500 nonindependent300plus200',bc['scope']=='known_business300_plus_training_replay200' and bc['independent_holdout'] is False and bman['independent_holdout'] is False)
daily_path=ROOT/'examples/agent/daily-qa-assistant/evals/reports/report-20260908-133013.json'
daily=read(daily_path)
daily_turns=[turn for case in daily['cases'] for turn in case['turns']]
daily_counts={'cases':len(daily['cases']),'turns':len(daily_turns),
              'cases_success':sum(case['case_success'] for case in daily['cases']),
              'turns_success':sum(turn['turn_success'] for turn in daily_turns),
              'rule_pass':sum(turn['rule_pass'] for turn in daily_turns),
              'judge_pass':sum(turn['judge_pass'] for turn in daily_turns)}
check('Daily QA25cases86turns,4case/50turn successes',daily_counts=={
    'cases':25,'turns':86,'cases_success':4,'turns_success':50,'rule_pass':70,'judge_pass':52})
frozen=read(ROOT/'configs/experiments/v1-policy-transfer/published-version.json')
fg=frozen['evaluation']['gate']
check('Historical15trial package14to15 passes,loop2thirds to1third',
      fg['baseline']['trials']==fg['candidate']['trials']==15 and
      fg['baseline']['passed']==14 and fg['candidate']['passed']==15 and
      fg['baseline']['invalid_loop_assessment']['mean']==2/3 and
      fg['candidate']['invalid_loop_assessment']['mean']==1/3)
cold=read(ROOT/'trace_output/v1-l4-coldstart/dataset/manifest.json')
check('Coldstart60 split30/15/15 and not L4 ready',cold['summary']['task_count']==60 and
      cold['summary']['splits']=={'train':30,'val':15,'holdout':15} and cold['summary']['l4_ready'] is False)
benchmark_inventory={}
for path in (ROOT/'benchmark/datasets').glob('*.json'):
    ds=read(path);cases=ds['cases']
    benchmark_inventory[path.name]={'cases':len(cases),'turns':sum(len(x.get('turns',[])) for x in cases),
                                   'split':dict(Counter(x.get('split','missing') for x in cases))}
local_missing=['trace_output/bfcl-v3','trace_output/tau2-retail-verified',
               'tools/trajectory_evolution/experiments/tau2-retail-verified/tau2_host.py',
               'examples/agent/business-simulator/appworld/runtime/venv/bin/python',
               'examples/agent/business-simulator/appworld/runtime/world','build/agentflowd',
               'tools/trajectory_evolution/build-v4/trajectory_evolution',
               'tools/trajectory_evolution/build/trajectory_evolution',
               'build/trajectory-evolution/v1-coldstart-input',
               'trace_output/v1-policy-transfer-20260915/evaluation/report.json',
               'trace_output/v1-l3-reproduction']

result={'current_head':subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),
        'verification':'read_only_compact_summary_reaggregation_no_models_no_rescoring',
        'original_native_provenance_verified':False,'historical_run_git_commit_verified':False,
        'AppWorld':{'baseline':bs,'frozen_seed':cs,'paired':dict(wlt),'by_source_view':subgroups,
             'dataset_counts':{k:len(v) for k,v in datasets.items()},
             'quality_absolute_gain':cs['quality']['mean']-bs['quality']['mean'],
             'quality_relative_gain':(338-322)/322,'loop_relative_reduction':(190-102)/190,
             'strict_needed_for_5pct':math.ceil(322*1.05),
             'extra_execution_attempts':{'L0':c['metrics']['L0_reused']['extra_execution_attempts'],
                                         'frozen_seed':c['metrics']['seed_candidate_typeerror_causal_current']['extra_execution_attempts']},
             'official_strict_counts':[323,339],'native_reports_missing':len(mapped),
             'delivery':{k:m[k] for k in ['status','release_status','goal_passed','independent_holdout','gepa_completion_claimed']},
             'tree':{'nodes':len(t['nodes']),'new_status':dict(new_status),'rounds':t['rounds'],
                     'optimizer_calls':t['model_calls'],'status':t['status'],'winner':t['winner'],
                     'node30_parent':t['nodes'][30]['parent'],'final_holdout_recorded_status':t['final_holdout']['status']}},
        'Business':{'final500':bc,'selected300_and_full400':b300},
        'Daily_QA':{'source':str(daily_path.relative_to(ROOT)),'meta':daily['meta'],
                    'summary':daily['summary'],'per_category':daily['per_category'],
                    'recomputed_counts':daily_counts,'evolution_comparison':False},
        'Historical_v1_frozen_gate':{'source':'configs/experiments/v1-policy-transfer/published-version.json',
                    'version':frozen['version'],'gate':fg,'original_trial_reports_present':False},
        'Coldstart_dataset':cold,
        'Benchmark_dataset_inventory':benchmark_inventory,
        'local_environment_checks':[{'path':p,'exists':(ROOT/p).exists()} for p in local_missing],
        'checks':checks}
(OUT/'实验指标复算_20261009.json').write_text(json.dumps(result,ensure_ascii=False,indent=2))
print(json.dumps({'checks_passed':len(checks),'views':{k:{'n':v['L0']['tasks'],'L0_strict':v['L0']['strict_passed'],
    'seed_strict':v['frozen_seed']['strict_passed'],'L0_loop':v['L0']['invalid_loop_count']['mean'],
    'seed_loop':v['frozen_seed']['invalid_loop_count']['mean'],'paired':v['paired']} for k,v in subgroups.items()},
    'seed_tasks_with_applications':cs['tasks_with_applications'],'local_environment_checks':result['local_environment_checks']},ensure_ascii=False))
