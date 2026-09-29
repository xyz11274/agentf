#!/usr/bin/env bash
set -u
ROOT="/root/lai-code/Agent/AgentFlow"
cd "$ROOT"
python3 - <<'PY'
import json
p='trace_output/business-simulator/gepa/workspace/agent-evaluation/state.json'
d=json.load(open(p))
print('jobs',list(d.get('jobs',{})))
for k,v in d.get('jobs',{}).items():
 print('JOB',k,'status=',v.get('status'),'progress=',v.get('progress'),'tree_status=',v.get('tree_status'))
PY
echo -n 'SIZE_KIB '; du -x -s -k trace_output/business-simulator | cut -f1
find trace_output/business-simulator -path '*/search/evaluations/*' -type f \( -name 'report.pb' -o -name 'results.jsonl' -o -name '*.md' \) -delete
echo -n 'SIZE_AFTER_KIB '; du -x -s -k trace_output/business-simulator | cut -f1
echo -n 'REMAINING_INTERMEDIATE '; find trace_output/business-simulator -path '*/search/evaluations/*' -type f \( -name 'report.pb' -o -name 'results.jsonl' -o -name '*.md' \) | wc -l
