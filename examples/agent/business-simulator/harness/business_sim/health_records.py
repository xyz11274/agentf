"""Versioned, patient-filtered records containing explicitly supplied fields."""
from datetime import datetime, timezone
import copy
import re
import uuid
from .live_information import result

LABELS = {"symptoms": "症状", "diagnosis": "诊断", "medication": "用药", "doctor_advice": "医嘱",
          "medical_history": "既往病史", "allergies": "过敏史", "visit_date": "就诊日期"}
PLACEHOLDERS = {"未提供", "尚未提供", "待补充", "未知", "需要更新", "请求更新", "等待用户", "暂无具体", "暂无具体信息"}


def query(state, args):
    rows = list(state['records'].values())
    for key in ('id', 'patient'):
        if args.get(key):
            rows = [r for r in rows if r.get(key) == args[key]]
    if not args.get('include_deleted'):
        rows = [r for r in rows if not r.get('deleted_at')]
    for field, comparison in (('start_date', lambda a,b:a>=b), ('end_date', lambda a,b:a<=b)):
        if args.get(field):
            rows = [r for r in rows if r.get('record_fields',{}).get('visit_date') and comparison(r['record_fields']['visit_date'], args[field])]
    if args.get('query'):
        rows = [r for r in rows if args['query'] in r.get('content','')]
    return result('已按条件读取健康档案。', {'records': rows}, operation_type='100153')


def save(state, args):
    fields = {key: value.strip() for key, value in args.get('record_fields', {}).items()
              if key in LABELS and isinstance(value, str) and value.strip()}
    # Match placeholders as complete values. "尚未服药" / "病因未知但持续咳嗽"
    # are clinical facts and must not disappear due to substring matching.
    fields = {k: v for k,v in fields.items() if re.sub(r'[。；;，,\s]+$', '', v) not in PLACEHOLDERS
              and not re.fullmatch(r'(?:待补充|未提供|尚未提供|暂无具体)(?:(?:诊断|症状|用药|医嘱|信息|资料)(?:信息|资料)?)?(?:[，,；;。：:].*(?:等待|补充|用户|提供|资料).*)?', v)}
    if not fields:
        return result('请提供实际健康资料；更新请求或待补信息不是档案，未写入。', status='needs_input', missing_fields=['record_fields'])
    if fields.get('visit_date'):
        from datetime import date
        date.fromisoformat(fields['visit_date'])
    rid = args.get('id') or 'record-'+uuid.uuid4().hex
    previous = state['records'].get(rid)
    if args.get('id') and (not previous or previous.get('deleted_at')):
        return result('健康档案不存在。', status='not_found')
    patient = args.get('patient') or (previous or {}).get('patient','当前用户')
    if previous and args.get('patient') and previous.get('patient',patient) != patient:
        return result('不能用更新档案修改所属患者，请新建该患者的档案。',status='failed')
    fields = {**(previous or {}).get('record_fields',{}), **fields}
    if not args.get('id') and not args.get('allow_duplicate'):
        duplicate = next((r for r in state['records'].values() if not r.get('deleted_at') and r.get('patient')==patient and r.get('record_fields')==fields), None)
        if duplicate:
            return result('相同健康资料已经保存，未重复创建。', {'healthRecord':duplicate}, operation_type='100152', duplicate=True)
    now = datetime.now(timezone.utc).isoformat()
    versions = copy.deepcopy((previous or {}).get('versions', []))
    if previous:
        versions.append({k: copy.deepcopy(v) for k,v in previous.items() if k != 'versions'})
    item = {'id':rid, 'record_fields':fields, 'content':'；'.join(LABELS[k]+'：'+v for k,v in fields.items()),
            'patient':patient, 'origin':'user_provided', 'created_at':(previous or {}).get('created_at',now),
            'updated_at':now, 'version':(previous or {}).get('version',0)+1, 'versions':versions,
            'source_evidence': args.get('source_evidence') or (previous or {}).get('source_evidence',[])}
    state['records'][rid] = item
    return result('已保存健康资料及版本记录。', {'healthRecord':item}, operation_type='100152')


def delete(state, args):
    item = state['records'].get(args.get('id'))
    if not item:
        return result('档案不存在。',status='not_found')
    item['deleted_at'] = datetime.now(timezone.utc).isoformat()
    return result('已删除当前档案；版本留作审计，不再用于普通查询与导诊。', {'id':item['id']}, operation_type='100152')
