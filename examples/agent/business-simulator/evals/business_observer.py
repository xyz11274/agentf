"""逐轮采集业务证据和意图观察；不向 Agent 暴露任何评分标签。"""
import json
import re


def compact(value, depth=0):
    """先按结构限长，再编码 JSON，避免截断 JSON 或膨胀裁判上下文。"""
    if depth > 5:
        return "[深层数据省略]"
    if isinstance(value, str):
        return value if len(value) <= 450 else value[:450] + "[省略]"
    if isinstance(value, list):
        return [compact(v, depth + 1) for v in value[:8]] + ([{"omitted_items": len(value)-8}] if len(value)>8 else [])
    if isinstance(value, dict):
        return {k: compact(v, depth + 1) for k, v in value.items()
                if k not in {"versions", "revisions", "evaluation_world", "hourly_usage", "history"}}
    return value


def execution_evidence(events):
    """只送本轮结果；状态、操作码和错误始终保留，不发送重复的整份状态库。"""
    from agentflow_runtime.security import redact_secrets
    rows, size = [], 0
    for event in events:
        answer = event.get('result', {})
        row = {"skill": event['skill'], "action": event['action'],
               "status": answer.get('status'), "operation_type": answer.get('operation_type'),
               "error": answer.get('error'), "simulated": answer.get('simulated'),
               "output": compact(answer.get('UserOutputObject', {})),
               "text": compact(answer.get('UserOutputText', ''))}
        encoded = redact_secrets(json.dumps(row, ensure_ascii=False))[0]
        if size + len(encoded) > 10000:
            # 即使单条结果过长，也先保留其执行状态，不能变成“完全没调用”。
            row.pop('output', None)
            row['output_omitted'] = True
            encoded = redact_secrets(json.dumps(row, ensure_ascii=False))[0]
        if size + len(encoded) > 12000:
            break
        rows.append(json.loads(encoded)); size += len(encoded)
    return {"events": rows, "total_events": len(events), "omitted_events": len(events)-len(rows),
            "scope": "current_turn_only"}


def observe_intents(turn, actual_skills):
    expected = turn.get('expected_skills')
    conditional = bool(re.search(r'如果|否则|若|满足.*才|符合.*才|不.*就|没有.*才',
                                 turn.get('user', '') + '\n' + turn.get('expected_behavior', '')))
    data = {"expected_intents": turn.get('expected_intents'), "expected_skills": expected,
            "actual_skills": actual_skills, "observation_only": True,
            "conditional": conditional, "basis": "dispatched_skills"}
    if expected is None or actual_skills is None:
        return {**data, "status": "unlabeled" if expected is None else "missing_trace"}
    wanted, actual = set(expected), set(actual_skills)
    # 同名 skill 重试只计一次；既不把重试算额外意图，也不把工具名当 skill。
    return {**data, "status": "conditional_review" if conditional else "measured",
            "matched": sorted(wanted & actual), "missing": sorted(wanted - actual),
            "extra": sorted(actual - wanted), "complete": wanted <= actual,
            "exact": wanted == actual}


class BusinessObserver:
    def __init__(self, store, scope):
        self.store, self.scope = store, scope
        self.cursor = len(store.events(scope))

    def __call__(self, result, turn, index):
        events = self.store.events(self.scope)
        current = events[self.cursor:]
        self.cursor = len(events)
        result['execution_evidence'] = execution_evidence(current)
        result['expected_intents'] = turn.get('expected_intents')
        result['expected_operation_types'] = turn.get('expected_operation_types')
        observation = observe_intents(turn, result.get('dispatched_skills'))
        result['intent_observation'] = observation
        actual = {str(e['result']['operation_type']) for e in current
                  if e['result'].get('status') == 'succeeded' and e['result'].get('operation_type')}
        # 条件任务不能对静态操作码列表作全量硬匹配，交给带执行证据的裁判。
        missing = set(map(str, turn.get('expected_operation_types') or [])) - actual
        result['business_rule_pass'] = not missing or observation['conditional']
        result['business_checks'] = {"actual_operation_types": sorted(actual),
                                     "missing_operation_types": sorted(missing),
                                     "conditional_review": observation['conditional']}
        result['requires_business_judge'] = True


def multi_intent_metrics(cases):
    rows = [t['intent_observation'] for c in cases for t in c['turns'] if 'intent_observation' in t]
    multiple = [r for r in rows if len(set(r.get('expected_skills') or [])) > 1]
    measured = [r for r in multiple if r['status'] == 'measured']
    matched = sum(len(r['matched']) for r in measured)
    expected = sum(len(set(r['expected_skills'])) for r in measured)
    actual = sum(len(set(r['actual_skills'])) for r in measured)
    def ratio(a, b):
        return round(a/b, 4) if b else None
    precision, recall = ratio(matched, actual), ratio(matched, expected)
    return {"observation_only": True, "basis": "unique_dispatched_skills", "multi_intent_turns": len(multiple),
            "measured_turns": len(measured), "conditional_turns": sum(r['conditional'] for r in multiple),
            "missing_trace_turns": sum(r['status'] == 'missing_trace' for r in multiple),
            "precision": precision, "recall": recall,
            "f1": ratio(2*matched, actual+expected),
            "complete_rate": ratio(sum(r['complete'] for r in measured), len(measured)),
            "exact_rate": ratio(sum(r['exact'] for r in measured), len(measured)),
            "missing_skill_count": sum(len(r['missing']) for r in measured),
            "extra_skill_count": sum(len(r['extra']) for r in measured)}
