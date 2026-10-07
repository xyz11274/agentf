"""Native manifests can bridge cleaned grader reports without inventing executions."""
from copy import deepcopy
import json
from pathlib import Path
import sys
import pytest
ROOT=Path(__file__).resolve().parents[4]
sys.path.insert(0,str(ROOT/'tools/trajectory_evolution/experiments/appworld'))
import compare
from test_appworld_compare_metrics import native_row


def fixture(root, *, sibling=False):
    stage=root/'stage';scope=stage/'shards/shard-00' if sibling else stage
    capture=scope/'capture/report.json'
    grade=scope/'regrade'
    final=(stage/'review-failures/shard-00/regrade/report.json') if sibling else scope/'regrade/retry-01/report.json'
    row=native_row('actual-task');row['evidence']['metrics']={'tokens':100,'llm_calls':2}
    nodes={capture:{'run_id':'capture','dataset_version':'native-dataset','trials':[row]},
           capture.with_name('manifest.json'):{'run_id':'capture','complete':True,'dataset_version':'native-dataset','files':{'report.json':'b'*64}},
           grade/'manifest.json':{'run_id':'grade','complete':True,'dataset_version':'native-dataset','files':{'report.json':'a'*64},
              'summary':{'reused_trials':0.0,'regraded_trials':1.0,'planned_trials':1.0,'not_started_trials':0.0,
                         'resume_source':{'run_id':'capture','report_sha256':'b'*64}}},
           final:{'run_id':'final','dataset_version':'native-dataset','trials':[row],
                  'summary':{'resume_source':{'run_id':'grade','report_sha256':'a'*64}}}}
    for path,value in nodes.items():path.parent.mkdir(parents=True,exist_ok=True);path.write_text(json.dumps(value))
    item=compare.compact_row(row,'shard-00');item.update(latency_ms=None,tokens=None,model_calls=None,execution_costs_complete=False)
    return scope,final,grade/'manifest.json',item,nodes


@pytest.mark.parametrize('sibling',[False,True])
def test_native_manifest_only_grade_bridges_to_actual_capture_without_another_attempt(tmp_path,sibling):
    scope,final,manifest,item,nodes=fixture(tmp_path,sibling=sibling)
    original={path:path.read_bytes() for path in nodes}
    result=compare.execution_cost_rows([item],final,scope)[0]
    assert result['execution_costs_complete'] is True
    assert (result['latency_ms'],result['tokens'],result['model_calls'])==(1000,100,2)
    assert result['execution_attempts']==1
    assert not manifest.with_name('report.json').exists()
    assert all(path.read_bytes()==data for path,data in original.items())


@pytest.mark.parametrize('change',['child_sha','parent_sha','incomplete','new_agent','wrong_run','wrong_dataset','missing_source'])
def test_forged_or_unknown_manifest_lineage_keeps_costs_unknown(tmp_path,change):
    scope,final,manifest,item,nodes=fixture(tmp_path)
    m=deepcopy(nodes[manifest])
    if change=='child_sha':m['files']['report.json']='c'*64
    if change=='parent_sha':m['summary']['resume_source']['report_sha256']='c'*64
    if change=='incomplete':m['complete']=False
    if change=='new_agent':m['summary']['regraded_trials']=0
    if change=='wrong_run':m['run_id']='unrelated'
    if change=='wrong_dataset':m['dataset_version']='different'
    if change=='missing_source':(scope/'capture/report.json').unlink()
    manifest.write_text(json.dumps(m))
    result=compare.execution_cost_rows([item],final,scope)[0]
    assert result['execution_costs_complete'] is False
    assert result['latency_ms'] is None and result['model_calls'] is None
