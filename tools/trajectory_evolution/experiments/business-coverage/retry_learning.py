#!/usr/bin/env python3
"""本轮实验维护命令：通过正式协议重试失败分析，不重新执行业务题目。"""
import argparse
import json
import selectors
import subprocess
import time
import urllib.request
import uuid

import coldstart


def retry(limit):
    env = coldstart.environment()
    root = coldstart.OUTPUT / 'evolution'
    failed = []
    for path in sorted((root / 'runs').glob('*.json')):
        run = coldstart.read(path)
        if run.get('evaluation_status') == 'failed':
            failed.append((path, run['identity']['run_id']))
    failed = failed[:limit]
    if not failed:
        print('NO_FAILED_RUNS', flush=True)
        return
    service = coldstart.read(coldstart.PLAN)['service']
    with (coldstart.OUTPUT / 'repair-service.log').open('ab', buffering=0) as log:
        process = subprocess.Popen([str(coldstart.BINARY), 'serve', '--config', service],
            cwd=coldstart.ROOT, env=env, stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE, stderr=log, text=True, start_new_session=True)
        try:
            with selectors.DefaultSelector() as selector:
                selector.register(process.stdout, selectors.EVENT_READ)
                if not selector.select(20):
                    raise RuntimeError('repair_service_not_ready')
            line = process.stdout.readline().strip()
            if 'listening=127.0.0.1:' not in line:
                raise RuntimeError('repair_service_endpoint_missing')
            port = line.split('listening=127.0.0.1:')[1].split()[0]
            opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
            for path, run_id in failed:
                payload = dict(operation='evaluation.retry', request_id=uuid.uuid4().hex, run_id=run_id)
                request = urllib.request.Request('http://127.0.0.1:'+port+'/v1/invoke',
                    json.dumps(payload).encode(), {'Content-Type': 'application/json',
                        'Authorization': 'Bearer '+env['EVOLUTION_BUSINESS_TOKEN']})
                with opener.open(request, timeout=10) as response:
                    answer = json.load(response)
                    if not answer.get('ok'):
                        raise RuntimeError('retry_not_accepted')
                print('RETRY', run_id, flush=True)
                while process.poll() is None:
                    current = coldstart.read(path)
                    if current.get('evaluation_status') not in ('running', 'pending'):
                        print('RESULT', run_id, current.get('evaluation_status'),
                              current.get('evaluation_error', ''), flush=True)
                        break
                    time.sleep(2)
        finally:
            process.terminate()
            try:
                process.wait(timeout=30)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--limit', type=int, default=1)
    retry(parser.parse_args().limit)
