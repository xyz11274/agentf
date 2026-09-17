"""Finite, auditable business workflows made from existing tool contracts."""
from datetime import datetime, timezone
import copy, hashlib, json, uuid
from .live_information import result

def execute(store,scope,args):
    from .contracts import SERVICES,contract,validate
    from .services import dispatch
    action=args['action'];key=args.get('key') or args.get('id')
    if action=='workflow_save':
        steps=args.get('steps',[])
        if not steps or len(steps)>20:return result('流程需要 1–20 个明确步骤。',status='needs_input',missing_fields=['steps'])
        for step in steps:
            if step['skill'] not in SERVICES or step['skill']=='custom-workflow':
                raise ValueError('流程只能调用已有业务 skill，不能递归执行自身')
            validate(step['args'],contract(step['skill'])[2])
        key=key or 'workflow-'+uuid.uuid4().hex
        def save(state):
            workflows=state.setdefault('workflows',{})
            old=workflows.get(key,{})
            item={'key':key,'title':args.get('title',key),'steps':steps,'version':old.get('version',0)+1}
            workflows[key]=item
            return result('可执行流程已保存；运行时才执行各步骤。',{'workflow':item},execution='saved')
        return store.run(scope,'custom-workflow',args,save)
    workflow=store.read(scope).get('workflows',{}).get(key)
    if not workflow:return result('未找到流程，请先 workflow_save。',status='not_found')
    request_id=args.get('request_id')
    if not request_id:return result('运行流程需要新的 request_id；重试沿用该值。',status='needs_input',missing_fields=['request_id'])
    run_id=hashlib.sha256((key+':'+str(workflow['version'])+':'+request_id).encode()).hexdigest()
    old=store.read(scope).get('workflow_runs',{}).get(run_id)
    if old:return {**old,'replayed':True}
    trace=[]
    for index,step in enumerate(workflow['steps']):
        call_args={**step['args'],'request_id':run_id+':'+str(index)}
        answer=dispatch(step['skill'],call_args,store=store,scope=scope)
        trace.append({'step':index+1,'skill':step['skill'],'args':step['args'],'result':answer})
        if answer['status']!='succeeded':break
    complete=len(trace)==len(workflow['steps']) and all(x['result']['status']=='succeeded' for x in trace)
    answer=result('流程各步骤已执行，请核对各项业务回执。' if complete else '流程在失败或缺参步骤停止；前面已执行的步骤不会自动撤销。',
                  {'run_id':run_id,'workflow':key,'trace':trace,'completed_steps':sum(t['result']['status']=='succeeded' for t in trace)},
                  status='succeeded' if complete else 'failed',execution='executed' if complete else 'partially_executed')
    def save_run(state):
        state.setdefault('workflow_runs',{})[run_id]=copy.deepcopy(answer)
        return result('已保存执行回执。')
    store.run(scope,'workflow-receipt',{'action':'save_receipt'},save_run)
    return answer

