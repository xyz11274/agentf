#!/usr/bin/env python3
"""Exercise the real daemon/coordinator/skills against a scripted local LLM server.

This checks protocol integration and persistence, NOT model reasoning accuracy.
Run with a Python environment containing agentflow-runtime, after building agentflowd.
"""
from __future__ import annotations

import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading
import time

PACKAGE = Path(__file__).resolve().parents[1]
REPO = PACKAGE.parents[2]
sys.path.insert(0, str(REPO / 'benchmark'))
sys.path.insert(0, str(PACKAGE / 'harness'))
from run_eval import RpcClient, check_daemon, send_turn, wait_turn_response
from business_sim.state import Store, scope_key


class ScriptedModel(BaseHTTPRequestHandler):
    turn = 1
    coordinator_calls = {}
    lock = threading.Lock()

    def log_message(self, *args):
        pass

    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        names = {t['function']['name'] for t in request.get('tools', [])}
        messages = request.get('messages', [])
        call, content = None, None
        if 'run_skills' in names:
            with self.lock:
                calls = self.coordinator_calls.get(self.turn, 0)
                self.coordinator_calls[self.turn] = calls + 1
            if not calls:
                call = ('run_skills', {'skills': [
                    {'name': 'smart-home', 'prompt': '打开客厅灯' if self.turn == 1 else '查询客厅灯'},
                    {'name': 'health-advice', 'prompt': '记录每天08:00吃维生素D的提醒' if self.turn == 1 else '查询已有提醒'},
                ], 'reason': 'scripted protocol integration test'})
            else:
                content = '模拟任务执行完毕。已核对灯状态和用药提醒。'
        elif any(m['role'] == 'tool' for m in messages):
            content = str(next(m for m in reversed(messages) if m['role'] == 'tool').get('content', ''))
        elif 'sim_smart_home' in names:
            call = ('sim_smart_home', {'action': 'control', 'commands': [{'device': '灯', 'room': '客厅', 'action': 'on'}]} if self.turn == 1 else {'action': 'query', 'device': '灯', 'room': '客厅'})
        elif 'sim_health_advice' in names:
            call = ('sim_health_advice', {'action': 'reminder_create', 'medicine': '维生素D', 'times': ['08:00']} if self.turn == 1 else {'action': 'reminder_query'})
        else:
            content = '模拟任务执行完毕。'
        msg = {'role': 'assistant', 'content': content}
        if call:
            msg['tool_calls'] = [{'id': f'call_{time.time_ns()}', 'type': 'function',
                                  'function': {'name': call[0], 'arguments': json.dumps(call[1], ensure_ascii=False)}}]
        finish = 'tool_calls' if call else 'stop'
        if request.get('stream'):
            delta = dict(msg)
            if call:
                delta['tool_calls'] = [{**c, 'index': i} for i, c in enumerate(msg['tool_calls'])]
            chunks = [{'id': 'local-smoke', 'object': 'chat.completion.chunk', 'model': 'local-scripted',
                       'choices': [{'index': 0, 'delta': delta, 'finish_reason': None}]},
                      {'id': 'local-smoke', 'object': 'chat.completion.chunk', 'model': 'local-scripted',
                       'choices': [{'index': 0, 'delta': {}, 'finish_reason': finish}]}]
            data = ''.join('data: ' + json.dumps(c, ensure_ascii=False) + '\n\n' for c in chunks) + 'data: [DONE]\n\n'
            body, content_type = data.encode(), 'text/event-stream'
        else:
            body = json.dumps({'id': 'local-smoke', 'object': 'chat.completion', 'model': 'local-scripted',
                'choices': [{'index': 0, 'message': msg, 'finish_reason': finish}],
                'usage': {'prompt_tokens': 1, 'completion_tokens': 1, 'total_tokens': 2}}, ensure_ascii=False).encode()
            content_type = 'application/json'
        self.send_response(200)
        self.send_header('Content-Type', content_type)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--daemon', type=Path, default=REPO / 'build/agentflowd')
    args = parser.parse_args()
    server = ThreadingHTTPServer(('127.0.0.1', 0), ScriptedModel)
    thread = threading.Thread(target=server.serve_forever, daemon=True); thread.start()
    out = Path(tempfile.mkdtemp(prefix='business-daemon-smoke-'))
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0)); port = sock.getsockname()[1]
    env = dict(os.environ)
    env.update(AF_LLM_API_KEY='local-test-key', AF_LLM_BASE_URL=f'http://127.0.0.1:{server.server_port}/v1',
               AF_LLM_MODEL='local-scripted', AF_COORDINATOR_VERSION='v1', AF_SKIP_DOTENV='1',
               AF_SESSIONLOG_ENABLED='1', AF_SESSIONLOG_DIR=str(out / 'sessionlogs'),
               AF_TRACE_OUTPUT_DIR=str(out / 'traces'), BUSINESS_SIM_DB=str(out / 'state.sqlite3'),
               BUSINESS_EXECUTION_MODE='fixture',BUSINESS_SIM_INFORMATION_MODE='fixture', PYTHON_PATH=str(Path(sys.executable).parent.parent),
               PATH=str(Path(sys.executable).parent) + os.pathsep + env.get('PATH', ''),
               NO_PROXY='127.0.0.1,localhost', no_proxy='127.0.0.1,localhost')
    config = json.loads((PACKAGE / 'daemon.json').read_text())
    config['server'].update(listen=f'127.0.0.1:{port}', uds_biz_path=str(out / 'biz.sock'), uds_ctrl_path=str(out / 'ctrl.sock'))
    config['daemon']['pid_file'] = str(out / 'daemon.pid')
    config['trace'] = {'output_dir': str(out / 'traces')}
    (out / 'config.json').write_text(json.dumps(config))
    print(f'Protocol smoke artifacts: {out}', flush=True)
    client = None
    with (out / 'daemon.log').open('w') as log:
        proc = subprocess.Popen([str(args.daemon), '--config', str(out / 'config.json'), '--package', str(PACKAGE)],
                                cwd=REPO, env=env, stdout=log, stderr=subprocess.STDOUT)
        try:
            for _ in range(100):
                if proc.poll() is not None:
                    raise RuntimeError('daemon exited during startup; inspect daemon.log')
                probe = RpcClient('127.0.0.1', port, timeout=3)
                try:
                    check_daemon(probe, 'business-simulator'); break
                except (OSError, RuntimeError):
                    time.sleep(.2)
                finally:
                    probe.close()
            else:
                raise RuntimeError('daemon startup timed out')
            print('Daemon ready', flush=True)
            client = RpcClient('127.0.0.1', port, timeout=90); client.connect()
            responses = []
            for turn, prompt in enumerate(['打开客厅的灯，同时记录每天08:00吃维生素D的提醒。', '查看客厅的灯和我的吃药提醒。'], 1):
                ScriptedModel.turn = turn
                send_turn(client, 'business-simulator', 'smoke-user', 'smoke-conversation', 'smoke-dialogue', prompt)
                result = wait_turn_response(client, time.time() + 90)
                responses.append(result)
                (out / 'responses.json').write_text(json.dumps(responses, ensure_ascii=False, indent=2))
                print(f'Turn {turn}: timeout={result.get("timed_out")} error={bool(result.get("error"))}', flush=True)
                assert not result.get('error') and not result.get('timed_out') and result.get('final_text'), result
            store = Store(out / 'state.sqlite3'); scope = scope_key('smoke-user', 'smoke-conversation')
            state, events = store.read(scope), store.events(scope)
            (out / 'evidence.json').write_text(json.dumps({'state': state, 'events': events}, ensure_ascii=False, indent=2))
            assert any(d['room'] == '客厅' and d['device'] == '灯' and d['power'] for d in state['devices'])
            assert len(state['reminders']) == 1
            assert {('smart-home', 'control'), ('smart-home', 'query'), ('health-advice', 'reminder_create'), ('health-advice', 'reminder_query')} <= {(e['skill'], e['action']) for e in events}
            assert list((out / 'sessionlogs').rglob('*.jsonl')), 'sessionlog hook did not record execution'
            print('PASS: real daemon -> coordinator -> parallel skills -> tools -> next-turn state + sessionlogs', flush=True)
            return 0
        finally:
            if client:
                client.close()
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill(); proc.wait()
            server.shutdown(); server.server_close(); thread.join(timeout=5)


if __name__ == '__main__':
    raise SystemExit(main())
