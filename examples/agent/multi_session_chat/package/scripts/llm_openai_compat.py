#!/usr/bin/env python3
import json, os, sys, urllib.request
from pathlib import Path

def load_local_env():
    env = {}
    script_root = Path(__file__).resolve().parent.parent
    for p in (script_root / ".env", script_root / "llm.env"):
        if not p.exists():
            continue
        for raw in p.read_text(encoding="utf-8").splitlines():
            line = raw.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            k, v = line.split("=", 1)
            env[k.strip()] = v.strip().strip('"').strip("'")
    return env

local_env = load_local_env()
base = (
    os.environ.get('AF_LLM_BASE_URL', '')
    or os.environ.get('LLM_BASE_URL', local_env.get('LLM_BASE_URL', 'https://api.openai.com/v1'))
)
api_key = (
    os.environ.get('AF_LLM_API_KEY', '')
    or os.environ.get('LLM_API_KEY', '')
    or os.environ.get('OPENAI_API_KEY', '')
    or os.environ.get('API_KEY', '')
    or local_env.get('AF_LLM_API_KEY', '')
    os.environ.get('LLM_API_KEY', '')
    or os.environ.get('OPENAI_API_KEY', '')
    or os.environ.get('API_KEY', '')
    or local_env.get('LLM_API_KEY', '')
    or local_env.get('OPENAI_API_KEY', '')
    or local_env.get('API_KEY', '')
)
model = (
    os.environ.get('AF_LLM_MODEL', '')
    or os.environ.get('LLM_MODEL', local_env.get('LLM_MODEL', 'gpt-4o-mini'))
)
timeout = float(
    os.environ.get('AF_LLM_TIMEOUT_SEC', '')
    or os.environ.get('LLM_TIMEOUT_SEC', local_env.get('LLM_TIMEOUT_SEC', '3'))
)
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
try:
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        data = json.loads(resp.read().decode('utf-8'))
    text = data.get('choices',[{}])[0].get('message',{}).get('content','')
    print(json.dumps({"text":text, "raw":data}, ensure_ascii=False))
except Exception as e:
    print(json.dumps({"text":"[mock] llm request failed; fallback response","error":str(e), "raw":{}}, ensure_ascii=False))
