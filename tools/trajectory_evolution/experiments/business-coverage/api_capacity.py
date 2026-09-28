#!/usr/bin/env python3
"""本地维护工具：测现有 API 的并发容量，不生成 Agent、不进入经验库。"""

import argparse
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone
import fcntl
import http.client
import json
import math
import os
import secrets
import threading
import time
from urllib.parse import urlsplit

import coldstart


def request(env, concurrent, barrier, input_chars, timeout):
    """直接连现有服务；不自动重试，从而不会把限流掩盖成成功。"""
    endpoint = urlsplit(env['OPENAI_BASE_URL'].rstrip('/') + '/chat/completions')
    if endpoint.scheme != 'https' or endpoint.hostname != 'ark.cn-beijing.volces.com':
        raise RuntimeError('unexpected_probe_endpoint')
    nonce = secrets.token_hex(6)
    # 使用独立合成文本，不额外发送业务数据。固定输入量让各并发档可比较；
    # nonce 放在最前面，降低整段输入缓存对容量测试的干扰。
    record = '测试记录：任务正常开始；检索得到资料；独立分支返回计划；这里只测接口吞吐，不评价真实业务。\n'
    context = (record * (input_chars // len(record) + 1))[:input_chars]
    prompt = (f'请求编号 {nonce}。以下仅为合成负载：\n{context}\n'
              '请仅返回 JSON 对象，包含 count=8，以及 items 数组；数组有八项，'
              '每项为 id（1到8）、status（ok）、reason（十个汉字以内的测试说明）。')
    body = json.dumps(dict(model=env['OPENAI_MODEL'], temperature=0, max_tokens=512,
                           thinking={'type': 'disabled'}, response_format={'type': 'json_object'},
                           messages=[{'role': 'user', 'content': prompt}]), ensure_ascii=False).encode()
    connection = http.client.HTTPSConnection(endpoint.hostname, endpoint.port, timeout=timeout)
    barrier.wait(timeout=15)
    started = time.monotonic()
    result = dict(http_status=0, ok=False, input_bytes=len(body), concurrency=concurrent)
    try:
        connection.request('POST', endpoint.path, body, {
            'Content-Type': 'application/json', 'Authorization': 'Bearer ' + env['OPENAI_API_KEY']})
        response = connection.getresponse()
        result['headers_seconds'] = round(time.monotonic() - started, 3)
        result['http_status'] = response.status
        retry_after = response.getheader('Retry-After', '')
        if retry_after.isdigit():
            result['retry_after_seconds'] = int(retry_after)
        data = response.read(262145)
        if len(data) > 262144:
            result['error'] = 'response_too_large'
        elif response.status != 200:
            # 不保存服务端错误正文或请求头，防止凭据被反射到报告。
            result['error'] = 'http_' + str(response.status)
        else:
            value = json.loads(data)
            choice = value['choices'][0]
            result['finish_reason'] = choice.get('finish_reason')
            content = json.loads(choice['message']['content'])
            result['ok'] = (choice.get('finish_reason') == 'stop' and content.get('count') == 8
                            and isinstance(content.get('items'), list) and len(content['items']) == 8)
            if not result['ok']:
                result['error'] = 'invalid_probe_response'
            result['usage'] = {key: val for key, val in value.get('usage', {}).items()
                               if key in ('prompt_tokens', 'completion_tokens', 'total_tokens')
                               and isinstance(val, (int, float))}
    except Exception as exc:
        # 异常仅保留类型，不输出可能包含请求信息的 str(exc)。
        result['error'] = type(exc).__name__
    finally:
        connection.close()
        result['seconds'] = round(time.monotonic() - started, 3)
    return result


def wave(env, level, input_chars, timeout):
    """同档请求同时开始，等待全部收尾后才换档，不形成无限积压。"""
    barrier = threading.Barrier(level)
    started = time.monotonic()
    with ThreadPoolExecutor(max_workers=level) as pool:
        futures = [pool.submit(request, env, level, barrier, input_chars, timeout) for _ in range(level)]
        rows = [future.result() for future in futures]
    return rows, time.monotonic() - started


def run(max_concurrency, waves, input_chars, timeout):
    """在实验写入锁内按倍增档位测量；首个不稳定档结束后停止加压。"""
    env = coldstart.environment()
    root = coldstart.OUTPUT
    with (root / 'runner.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        if coldstart.read(root / 'status.json').get('status') == 'running':
            raise RuntimeError('training_not_paused')
        output = dict(started_at=datetime.now(timezone.utc).isoformat(), model=env['OPENAI_MODEL'],
                      endpoint=env['OPENAI_BASE_URL'], input_chars=input_chars, waves=waves,
                      max_concurrency=max_concurrency, timeout_seconds=timeout,
                      thinking='disabled', automatic_retries=0, levels=[])
        level = 1
        while level <= max_concurrency:
            rows, wall = [], 0.0
            for _ in range(waves):
                current, elapsed = wave(env, level, input_chars, timeout)
                rows.extend(current)
                wall += elapsed
                if any(not item['ok'] for item in current):
                    break
            latencies = sorted(item['seconds'] for item in rows)
            success = sum(item['ok'] for item in rows)
            item = dict(concurrency=level, requests=len(rows), succeeded=success,
                        statuses=dict(Counter(str(row['http_status']) for row in rows)),
                        errors=dict(Counter(row.get('error') for row in rows if not row['ok'])),
                        wall_seconds=round(wall, 3), requests_per_second=round(success / wall, 4),
                        p50_seconds=latencies[len(latencies)//2],
                        p95_seconds=latencies[max(0, math.ceil(len(latencies)*0.95)-1)],
                        prompt_tokens=sum(row.get('usage', {}).get('prompt_tokens', 0) for row in rows),
                        completion_tokens=sum(row.get('usage', {}).get('completion_tokens', 0) for row in rows),
                        stable=success == len(rows), samples=rows)
            output['levels'].append(item)
            stable = [entry for entry in output['levels'] if entry['stable']]
            output['max_successful_concurrency'] = max((entry['concurrency'] for entry in stable), default=0)
            output['best_throughput_concurrency'] = max(stable, key=lambda entry: entry['requests_per_second'])['concurrency'] if stable else 0
            coldstart.save(root / 'api-capacity.json', output)
            print(json.dumps({key: value for key, value in item.items() if key != 'samples'}, ensure_ascii=False), flush=True)
            if not item['stable']:
                output['stop_reason'] = 'unstable_level'
                break
            level *= 2
        else:
            output['stop_reason'] = 'tested_ceiling_reached_not_provider_limit'
        output['finished_at'] = datetime.now(timezone.utc).isoformat()
        coldstart.save(root / 'api-capacity.json', output)
        return output


if __name__ == '__main__':
    os.umask(0o077)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--max-concurrency', type=int, choices=(1, 2, 4, 8, 16, 32, 64), default=32)
    parser.add_argument('--waves', type=int, choices=(1, 2, 3), default=2)
    parser.add_argument('--input-chars', type=int, default=6000)
    parser.add_argument('--timeout', type=int, default=120)
    args = parser.parse_args()
    if not 256 <= args.input_chars <= 24000 or not 10 <= args.timeout <= 180:
        parser.error('输入量或超时超出本轮容量测试预算')
    run(args.max_concurrency, args.waves, args.input_chars, args.timeout)
