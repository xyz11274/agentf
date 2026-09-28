"""Durable browser operation receipts: a retry must not repeat an uncertain click."""
import hashlib
import json
from .live_information import result

def execute(store,scope,skill,args):
    from .browser import execute as browse
    from .task_context import current_task
    receipt_skill = skill + ':' + hashlib.sha256(current_task().encode()).hexdigest() if current_task() else skill
    request_id=args.get('request_id')
    if not request_id:
        return browse(store,scope,skill,args)
    fingerprint=hashlib.sha256(json.dumps(args,sort_keys=True,ensure_ascii=False).encode()).hexdigest()
    with store.connect() as db:
        db.execute('CREATE TABLE IF NOT EXISTS browser_operations(scope TEXT,skill TEXT,request_id TEXT,fingerprint TEXT,result TEXT,PRIMARY KEY(scope,skill,request_id))')
        db.execute('BEGIN IMMEDIATE')
        old=db.execute('SELECT fingerprint,result FROM browser_operations WHERE scope=? AND skill=? AND request_id=?',(scope,receipt_skill,request_id)).fetchone()
        if old:
            if old[0]!=fingerprint:raise ValueError('request_id was already used with different arguments')
            if old[1]:
                return {**json.loads(old[1]),'replayed':True}
            return result('这次操作已开始但结果尚不确定。请先 browser_observe 回读，不能重复提交。',
                          status='needs_input',operation_id=request_id,execution='not_verified',retryable=False)
        db.execute('INSERT INTO browser_operations VALUES (?,?,?,?,NULL)',(scope,receipt_skill,request_id,fingerprint))
    answer={**browse(store,scope,skill,args),'operation_id':request_id}
    with store.connect() as db:
        db.execute('UPDATE browser_operations SET result=? WHERE scope=? AND skill=? AND request_id=?',
                   (json.dumps(answer,ensure_ascii=False),scope,receipt_skill,request_id))
    return answer
