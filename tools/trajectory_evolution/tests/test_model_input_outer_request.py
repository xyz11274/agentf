"""Actual native judge projection preserves new outer requests without new calls."""
import json
from pathlib import Path
import shutil
import subprocess
import sys

import pytest

ROOT = Path(__file__).resolve().parents[3]


@pytest.fixture(scope='module')
def projector(tmp_path_factory):
    compiler = shutil.which('g++')
    include = next((p for p in (Path(sys.prefix)/'include', Path('/usr/include'),
                    Path('/root/miniconda3/envs/agentflow-dev/include'))
                    if (p/'rapidjson/document.h').is_file()), None)
    if compiler is None or include is None:
        pytest.skip('native header compiler dependency unavailable')
    root = tmp_path_factory.mktemp('native-projection')
    source = root/'projection.cpp'
    header = ROOT/'tools/trajectory_evolution/src/model/model_input.h'
    source.write_text('''#include <iostream>
#include <iterator>
#include "''' + str(header) + '''"
int main(int argc, char **argv) {
  std::string input((std::istreambuf_iterator<char>(std::cin)), std::istreambuf_iterator<char>());
  rapidjson::Document doc; doc.Parse(input.data(), input.size());
  if (doc.HasParseError()) return 2;
  agentflow::trajectory::model_input::task_evidence_view(doc, std::string(argv[1]) == "1",
      true, true, std::string(argv[2]) == "1");
  std::cout << agentflow::trajectory::model_input::bytes(doc);
}
''')
    executable = root/'projection'
    subprocess.run([compiler, '-std=c++17', '-O0', '-I'+str(include), str(source), '-o', str(executable)],
                   check=True, capture_output=True)
    def project(evidence, *, keep=False, include_models=False):
        result = subprocess.run([str(executable),str(int(include_models)),str(int(keep))],
            input=json.dumps(evidence), text=True, capture_output=True, check=True)
        return json.loads(result.stdout)
    return project


def event(eid, kind, *, payload=None, activity=None, execution='main'):
    row={'event_id':eid,'kind':kind,'execution_id':execution,'step_id':eid+'-step',
         'actor_id':'unspecified','loop_id':'main','iteration':'1'}
    if payload is not None: row['input']=payload
    if activity: row['attributes']={'activity_kind':activity,'semantic':'custom'}
    return row


def envelope(turn, observation):
    return {'system':'Same public task','messages':[{'role':'user','content':observation}],'turn':turn}


def trace(*events):
    return {'trace':{'identity':{'run_id':'fixture'},'events':[
        event('state','step.start',payload={'progress':0}),*events,
        event('start','action.start'),event('end','action.end')]}}


def inputs(projected):
    return {r['event_id']:r['input'] for r in projected['trace']['events'] if 'input' in r}


def test_new_outer_request_keeps_actual_turn_and_messages_even_after_control_input(projector):
    first,second=envelope(0,'First request'),envelope(1,'New real observation')
    evidence=trace(event('outer0','step.start',payload=first,activity='agent-turn'),
                   event('outer1','step.start',payload=second,activity='agent-turn'))
    old=inputs(projector(evidence));new=inputs(projector(evidence,keep=True))
    assert 'outer0' not in old and 'outer1' not in old
    assert new['outer0']==first and new['outer1']==second
    assert new['state']==old['state']


def test_identical_request_deduplicates_without_collapsing_new_observation_or_execution(projector):
    request=envelope(1,'Observed value')
    evidence=trace(event('outer0','step.start',payload=request,activity='agent-turn'),
                   event('duplicate','step.start',payload=request,activity='agent-turn'),
                   event('other-execution','step.start',payload=request,activity='agent-turn',execution='other'),
                   event('new','step.start',payload=envelope(1,'Changed observed value'),activity='agent-turn'))
    result=inputs(projector(evidence,keep=True))
    assert 'outer0' in result and 'duplicate' not in result
    assert result['other-execution']==request and result['new']['messages'][0]['content']=='Changed observed value'


def test_business_role_messages_and_regular_state_inputs_keep_existing_semantics(projector):
    evidence=trace(event('message1','step.start',payload={'role':'user','content':'Business request'}),
                   event('message2','step.start',payload={'role':'user','content':'Updated business request'}),
                   event('state2','step.start',payload={'progress':1}))
    assert projector(evidence,keep=True)==projector(evidence)
    assert {'message1','message2'} <= set(inputs(projector(evidence)))
    assert 'state2' not in inputs(projector(evidence))


def test_default_model_envelopes_projection_is_unchanged(projector):
    evidence=trace(event('outer','step.start',payload=envelope(2,'Observed'),activity='agent-turn'))
    assert projector(evidence,keep=True,include_models=True)==projector(evidence,include_models=True)


def test_unrecognized_outer_shape_does_not_expand_generic_business_inputs(projector):
    evidence=trace(event('missing-turn','step.start',payload={'system':'s','messages':[]},activity='agent-turn'),
                   event('state-envelope','step.start',payload=envelope(2,'v'),activity='state'))
    assert projector(evidence,keep=True)==projector(evidence)
