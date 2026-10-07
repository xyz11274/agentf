"""Offline native proof of actual dispatch dependencies; no automatic keep."""
import copy
import json
from pathlib import Path
import shutil
import subprocess
import sys
import pytest

ROOT = Path(__file__).resolve().parents[3]

@pytest.fixture(scope='module')
def check(tmp_path_factory):
    include = next((p for p in (Path(sys.prefix)/'include', Path('/usr/include'),
                    Path('/root/miniconda3/envs/agentflow-dev/include'))
                    if (p/'rapidjson/document.h').is_file()), None)
    compiler = shutil.which('g++')
    if not include or not compiler:
        pytest.skip('native header compiler unavailable')
    root=tmp_path_factory.mktemp('dispatch-dependency')
    source=root/'check.cpp'
    source.write_text('''#include <iostream>
#include <iterator>
#include "''' + str(ROOT/'tools/trajectory_evolution/src/model/model_input.h') + '''"
int main() {
 std::string raw((std::istreambuf_iterator<char>(std::cin)), std::istreambuf_iterator<char>());
 rapidjson::Document d; d.Parse(raw.data(), raw.size());
 rapidjson::Document e; e.CopyFrom(d["evidence"], e.GetAllocator());
 agentflow::trajectory::model_input::task_evidence_view(e, false, true, true, true);
 auto conflicts=agentflow::trajectory::model_input::dispatch_dependency_conflicts(
     d["review"], d["inventory"], e, d.GetAllocator());
 std::cout << agentflow::trajectory::model_input::bytes(conflicts);
}
''')
    binary=root/'check'
    subprocess.run([compiler,'-std=c++17','-O0','-I'+str(include),str(source),'-o',str(binary)],
                   capture_output=True,check=True)
    def invoke(evidence, review, inventory):
        p=subprocess.run([str(binary)], input=json.dumps(dict(evidence=evidence,review=review,inventory=inventory)),
                         capture_output=True,text=True,check=True)
        return json.loads(p.stdout)
    return invoke


def fixture():
    events=[]
    def event(eid, actor, execution, parent, *, prompt='Review invoice', output=None, outer=False):
        row=dict(event_id=eid,kind='step.start' if outer else 'action.end',actor_id=actor,
                 execution_id=execution,loop_id=execution,iteration='1',timestamp_us=len(events)+1,
                 caused_by=[parent] if parent else [])
        row['attributes']=dict(activity_kind='agent-turn') if outer else dict(semantic='model',executed=True,
            extensions=dict(source_event_name='llm.request.prepared',exec_skill_key='business-skill-key'))
        if not outer:
            row['input']=dict(messages=[dict(role='system',content='## Skill: invoice-review\nPublic task'),
                                       dict(role='user',content=prompt)])
            row['output']=output or dict(choices=[dict(content='Necessary invoice result')])
        events.append(row)
    event('outer','host','main',None,outer=True)
    event('dispatch','coordinator_decide','coordinator','outer',output=dict(choices=[dict(tool_calls=[dict(
        tool_name='run_skills',tool_call_id='call',arguments=dict(skills=[dict(name='invoice-review',prompt='Review invoice')]))])]))
    event('child','skill_executor','worker','outer')
    evidence=dict(trace=dict(identity=dict(run_id='business-run'),events=events))
    inventory=dict(dispatch=dict(run_id='business-run',event_ids=['dispatch']),
                   child=dict(run_id='business-run',event_ids=['child']))
    review=dict(iteration_reviews=[dict(iteration_id='dispatch',decision='waste'),dict(iteration_id='child',decision='keep')])
    return evidence,review,inventory


def test_actual_business_dispatch_with_retained_child_rejects_without_assigning_score(check):
    evidence,review,inventory=fixture()
    conflicts=check(evidence,review,inventory)
    assert len(conflicts)==1
    assert conflicts[0]['iteration_id']=='dispatch'
    assert conflicts[0]['retained_iteration_id']=='child'
    assert review['iteration_reviews'][0]['decision']=='waste'


@pytest.mark.parametrize('dispatch,child',[('waste','waste'),('keep','keep'),('waste','uncertain')])
def test_known_invalid_children_and_healthy_or_uncertain_reviews_are_not_forced_keep(check,dispatch,child):
    evidence,review,inventory=fixture()
    review['iteration_reviews'][0]['decision']=dispatch
    review['iteration_reviews'][1]['decision']=child
    assert check(evidence,review,inventory)==[]


@pytest.mark.parametrize('change',['other_parent','other_prompt','other_run','ambiguous'])
def test_unbound_or_ambiguous_requests_never_borrow_another_contribution(check,change):
    evidence,review,inventory=fixture()
    events=evidence['trace']['events']
    if change=='other_parent': events[-1]['caused_by']=[]
    if change=='other_prompt': events[-1]['input']['messages'][-1]['content']='A different public request'
    if change=='other_run': inventory['child']['run_id']='another-business-run'
    if change=='ambiguous':
        duplicate=copy.deepcopy(events[-1]);duplicate.update(event_id='other-child',timestamp_us=4);events.append(duplicate)
    assert check(evidence,review,inventory)==[]


def test_real_frozen_capture_rejects_thirteen_contradictory_coordinator_reviews(check):
    path=ROOT/'trace_output/appworld/gepa/seed-search-67-stage32/shards/shard-00/capture/trials/2e232f9c40dc95e91b6f2324cdc3d598.json'
    if not path.is_file(): pytest.skip('local actual capture unavailable')
    evidence=json.loads(path.read_text())['evidence']
    trace=evidence['trace'];run=trace['identity']['run_id']
    by_id={row['event_id']:row for row in trace['events']}
    inventory={}
    for row in evidence['loop_review']['iteration_reviews']:
        anchor=by_id[row['evidence_event_ids'][0]]
        keys=('execution_id','loop_id','iteration')
        ids=[e['event_id'] for e in trace['events'] if all(str(e.get(k))==str(anchor.get(k)) for k in keys)]
        inventory[row['iteration_id']]=dict(run_id=run,event_ids=ids)
    conflicts=check(evidence,evidence['loop_review'],inventory)
    assert len(conflicts)==13
    assert any(row['dispatch_event_id']=='event-44' and row['skill_event_id']=='event-52' for row in conflicts)


def test_real_known_failed_doc_retry_can_discard_dispatch_and_child_together(check):
    path=ROOT/'trace_output/appworld/gepa/l0-current-stage100/regrade/retry-05/trials/4bc5091ac9bad327088cc9f5bebe24cb.json'
    if not path.is_file(): pytest.skip('local actual regrade unavailable')
    evidence=json.loads(path.read_text())['evidence']
    trace=evidence['trace'];run=trace['identity']['run_id']
    by_id={row['event_id']:row for row in trace['events']}
    # The actual preceding request already failed on this same public API;
    # the final request produces the exact same observation. This is a
    # structural dependency check, not a replacement for semantic grading.
    assert by_id['event-138']['output']['observation']==by_id['event-161']['output']['observation']
    inventory={key:dict(run_id=run,event_ids=[eid]) for key,eid in
               [('dispatch','event-149'),('child','event-156')]}
    review=dict(iteration_reviews=[dict(iteration_id=key,decision='waste')
                                  for key in inventory])
    assert check(evidence,review,inventory)==[]
    review['iteration_reviews'][1]['decision']='keep'
    conflicts=check(evidence,review,inventory)
    assert len(conflicts)==1
    assert conflicts[0]['dispatch_event_id']=='event-149'
    assert conflicts[0]['skill_event_id']=='event-156'
