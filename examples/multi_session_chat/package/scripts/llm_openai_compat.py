#!/usr/bin/env python3
import json, os, sys, urllib.request
base = os.environ.get('LLM_BASE_URL', 'https://api.openai.com/v1')
api_key = os.environ.get('LLM_API_KEY', '')
model = os.environ.get('LLM_MODEL', 'gpt-4o-mini')
prompt = sys.stdin.read()
if not api_key:
    print(json.dumps({"text":"[mock] no api key; fallback response","raw":{}}))
    raise SystemExit(0)
url = base.rstrip('/') + '/chat/completions'
body = {
  'model': model,
  'messages': [
    {'role': 'system', 'content': 'Return concise JSON when possible.'},
    {'role': 'user', 'content': prompt}
  ],
  'temperature': 0.2
}
req = urllib.request.Request(url, data=json.dumps(body).encode('utf-8'), method='POST')
req.add_header('Content-Type', 'application/json')
req.add_header('Authorization', f'Bearer {api_key}')
with urllib.request.urlopen(req, timeout=20) as resp:
    data = json.loads(resp.read().decode('utf-8'))
text = data.get('choices',[{}])[0].get('message',{}).get('content','')
print(json.dumps({"text":text, "raw":data}, ensure_ascii=False))
