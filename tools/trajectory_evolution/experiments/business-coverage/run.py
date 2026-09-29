#!/usr/bin/env python3
"""本地全场景实验排程；只调用原生评测 CLI，不实现 Agent 或另写评分器。"""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict, deque
from copy import deepcopy
from datetime import datetime, timezone
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[4]
CONFIG = Path(__file__).resolve().parent
DATASET = CONFIG/'datasets/source'
SCENARIO_CONFIG = CONFIG/'scenario-deployment'
SCENARIO_PLAN = CONFIG/'scenario-plan.json'
OUTPUT = ROOT/'trace_output/business-coverage'
BINARY = ROOT/'build/trajectory-evolution/trajectory_evolution'
PACKAGE = ROOT/'examples/agent/business-simulator'
sys.path[:0] = [str(ROOT/'benchmark'), str(PACKAGE/'evals')]


def read(path):
    return json.loads(path.read_text(encoding='utf-8'))


def save(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name+'.new')
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2)+'\n', encoding='utf-8')
    temporary.replace(path)


def tasks():
    return [json.loads(line) for split in ('train', 'val', 'holdout')
            for line in (DATASET/f'{split}.jsonl').read_text().splitlines()]


def dimensions(rows):
    result = {key: set() for key in ('categories', 'scenarios', 'scenario_labels', 'targets', 'effects', 'target_combinations', 'turn_counts')}
    for task in rows:
        result['categories'].add(task['category'])
        result['scenarios'].add(tuple(task['attributes']['product_source']['scenarios']))
        result['scenario_labels'].update(task['attributes']['product_source']['scenarios'])
        result['turn_counts'].add(1+len(task.get('followup_inputs', [])))
        for turn in task['attributes']['turn_expectations']:
            targets = turn.get('expected_targets') or []
            result['targets'].update(targets)
            result['effects'].update(turn.get('expected_effects') or [])
            if len(targets) > 1:
                result['target_combinations'].add(tuple(sorted(targets)))
    return result


def representatives(rows):
    """覆盖源表的场景标签、能力和边界轮数，不穷举每种标签组合或每条用例。"""
    def features(task):
        return {(key, value) for key, values in dimensions([task]).items()
                if key != 'scenarios' for value in values}
    uncovered = set().union(*(features(task) for task in rows))
    pool = sorted(rows, key=lambda task: task['task_id'])
    selected = []
    while uncovered:
        # 优先覆盖缺口，同等覆盖时优先训练集和较短对话。完整会话始终不拆分。
        candidate = max(pool, key=lambda task: (len(features(task) & uncovered),
            {'train': 2, 'val': 1, 'holdout': 0}[task['split']], -len(task.get('followup_inputs', []))))
        gain = features(candidate) & uncovered
        if not gain:
            raise RuntimeError('场景覆盖存在缺口')
        selected.append(candidate); pool.remove(candidate); uncovered -= gain
    return selected


def environment():
    from run_eval import load_env
    load_env(PACKAGE/'.env')
    # 只清理本实验进程及其子进程环境；pass_env 按变量名转发时也无法重新注入代理。
    for key in ('HTTP_PROXY', 'HTTPS_PROXY', 'ALL_PROXY',
                'http_proxy', 'https_proxy', 'all_proxy', 'BUSINESS_BROWSER_PROXY'):
        os.environ.pop(key, None)
    os.environ['NO_PROXY'] = '*'
    os.environ['no_proxy'] = '*'
    # 凭据只在进程环境中流转，不复制到配置、命令行或报告。
    provider_base = os.environ['AF_LLM_BASE_URL'].rstrip('/')
    assert provider_base in {
        'https://ark.cn-beijing.volces.com/api/coding/v3',
        'https://ark.cn-beijing.volces.com/api/plan/v3',
    }
    # The active Volcano account may expose the coding-plan model while an
    # older checkpoint was prepared with the flash model.  Keep the provider
    # boundary strict, but accept either model so a quota/account switch can
    # resume without failing before the checkpoint runner starts.
    assert os.environ['AF_LLM_MODEL'] in {'deepseek-v4-flash', 'my-coding-plan'}
    os.environ['OPENAI_API_KEY'] = os.environ['AF_LLM_API_KEY']
    # OPENAI_BASE_URL 维持原树冻结时的评分来源标识；实际裁判传输入口单独记录在
    # candidate execution fingerprint 中，从而可以更换额度账户但不伪造旧评分。
    os.environ['OPENAI_BASE_URL'] = 'https://ark.cn-beijing.volces.com/api/coding/v3'
    os.environ['TRAJECTORY_MODEL_ENDPOINT_OVERRIDE'] = provider_base + '/chat/completions'
    os.environ['OPENAI_MODEL'] = os.environ['AF_LLM_MODEL']
    os.environ['AF_LLM_THINKING_MODE'] = 'disabled'
    # Keep the client boundary long enough for the largest business turn;
    # successful-request latency is measured from the actual response and is
    # not padded by this timeout.
    os.environ['AF_LLM_TIMEOUT'] = '300'
    # 主 Agent 与裁判共享供应商额度；瞬时 429 在总超时内指数退避，不能直接
    # 记成业务失败。
    os.environ['AF_LLM_MAX_RETRIES'] = '4'
    os.environ['AF_LLM_RETRY_BASE_DELAY'] = '2'
    # 只限制执行 worker 数；该值不改变数据集、裁判、模型或评分口径。
    os.environ['TRAJECTORY_EVALUATION_PARALLELISM'] = '16'
    # Keep Python and native libcurl/OpenSSL in one environment.  Starting
    # from base Python can preload an incompatible libssl before business
    # evaluation launches its agentflow-dev child.
    interpreter = Path(os.environ.get('AGENTFLOW_PYTHON',
                          '/root/miniconda3/envs/agentflow-dev/bin/python3'))
    if interpreter.is_file():
        os.environ['AGENTFLOW_PYTHON'] = str(interpreter)
        os.environ['PATH'] = str(interpreter.parent) + os.pathsep + os.environ.get('PATH', '')
        runtime_lib = interpreter.parent.parent / 'lib'
        if runtime_lib.is_dir():
            os.environ['LD_LIBRARY_PATH'] = str(runtime_lib) + os.pathsep + os.environ.get('LD_LIBRARY_PATH', '')
    return os.environ.copy()


def prepare():
    from prepare_dataset import prepare_configuration
    if SCENARIO_PLAN.exists() or OUTPUT.exists():
        raise RuntimeError('已有实验计划或结果，不自动覆盖')
    environment()
    subprocess.run([str(BINARY), 'dataset', 'check', str(DATASET)], check=True)
    deployment = prepare_configuration(SCENARIO_CONFIG, DATASET)
    base = read(deployment/'evaluation.json')
    # 新计划显式使用分批目标评分，避免部署模板仍是旧配置时悄悄回到整题二元评分。
    base['judge'].update(assessment_mode='objectives', max_batch_turns=4,
        batch_input_bytes=49152, max_evidence_lookups=2, evidence_field_bytes=2048,
        goal_prompt=os.path.relpath(ROOT/'tools/trajectory_evolution/config/prompts/task-goals.txt', deployment))
    rows = representatives(tasks())
    assert len(rows) == len({t['task_id'] for t in rows})
    batches = []
    # 每个 split 内按一级分类轮转，不让初期全部集中在 GUI；不改变原数据或轮次顺序。
    for split in ('train', 'val', 'holdout'):
        buckets = defaultdict(deque)
        for task in sorted((t for t in rows if t['split'] == split), key=lambda t: t['task_id']):
            buckets[task['category']].append(task)
        ordered = []
        while any(buckets.values()):
            for category in sorted(buckets):
                if buckets[category]:
                    ordered.append(buckets[category].popleft())
        current, turns = [], 0
        for task in ordered:
            count = 1+len(task.get('followup_inputs', []))
            if current and (len(current) >= 8 or turns+count > 20):
                batches.append((split, current)); current, turns = [], 0
            current.append(task); turns += count
        if current:
            batches.append((split, current))
    plan = []
    for index, (split, group) in enumerate(batches, 1):
        name = f'{index:03d}-{split}'
        config = deepcopy(base)
        config.update(splits=[split], task_ids=[t['task_id'] for t in group],
                      max_trials=len(group), parallelism=2)
        config['judge']['max_model_calls'] = 4*sum(1+len(t.get('followup_inputs', [])) for t in group)
        path = deployment/f'{name}.json'
        save(path, config)
        plan.append({'name': name, 'split': split, 'config': str(path.relative_to(ROOT)), 'task_ids': config['task_ids']})
    known = dimensions(rows)
    save(SCENARIO_PLAN, {'selection': 'scenario_representatives_v1',
        'dataset_version': read(DATASET/'manifest.json')['metadata']['dataset_version'],
        'dataset_files': read(DATASET/'manifest.json')['files'], 'tasks': len(rows),
        'turns': sum(1+len(t.get('followup_inputs', [])) for t in rows), 'concurrency': 4,
        'coverage': {k: sorted(v) for k, v in known.items()}, 'batches': plan,
        'mode': 'L0 diagnostic evaluation only; no memory import, no GEPA',
        'holdout_use': 'diagnostic_only_not_candidate_search',
        'binary_sha256': hashlib.sha256(BINARY.read_bytes()).hexdigest()})
    print(json.dumps({'tasks': len(rows), 'batches': len(plan),
                      'coverage': {k: len(v) for k,v in known.items()}}, ensure_ascii=False))


def inspect_trial(trial, task):
    evidence = trial.get('evidence', {})
    traces = evidence.get('traces') or ([evidence['trace']] if evidence.get('trace') else [])
    problems = []
    expected = 1+len(task.get('followup_inputs', []))
    if len(traces) != expected:
        problems.append('trace_turn_count_mismatch')
    for trace in traces:
        events = trace['events']; ids = {e['event_id'] for e in events}
        kinds = Counter(e['kind'] for e in events)
        if not trace.get('attributes', {}).get('capture_complete'):
            problems.append('capture_incomplete')
        if len(ids) != len(events) or kinds['run.start'] != 1 or kinds['run.end'] != 1:
            problems.append('event_identity_or_run_lifecycle')
        if any(ref not in ids for e in events for ref in e.get('caused_by', [])):
            problems.append('dangling_cause')
        for kind in ('step', 'action'):
            if Counter(e.get(kind+'_id') for e in events if e['kind'] == kind+'.start') != Counter(
                    e.get(kind+'_id') for e in events if e['kind'] == kind+'.end'):
                problems.append(kind+'_lifecycle')
    result = {k: trial.get(k) for k in ('task_id', 'split', 'category', 'verdict', 'execution_status', 'error_code', 'duration_ms', 'metrics')}
    result.update(expected_turns=expected, captured_turns=len(traces), trace_issues=sorted(set(problems)),
        graded_turns=len(evidence.get('turn_evaluation', [])),
        judge_errors=[c.get('reason') for c in trial.get('checks', []) if c.get('check_id') == 'model' and c.get('status') == 'error'],
        invalid_loops=evidence.get('loop_review', {}).get('count'))
    return result


def run_business_coverage():
    plan = read(SCENARIO_PLAN)
    # 旧的全量 800 题计划不得被新版命令意外启动，必须先重新准备代表性覆盖计划。
    if plan.get('selection') != 'scenario_representatives_v1':
        raise RuntimeError('旧全量计划已停用；先准备代表性场景覆盖计划，不允许直接执行全量实验')
    env = environment()
    for name, digest in plan['dataset_files'].items():
        assert hashlib.sha256((DATASET/name).read_bytes()).hexdigest() == digest, 'dataset_changed'
    assert hashlib.sha256(BINARY.read_bytes()).hexdigest() == plan['binary_sha256'], 'binary_changed'
    OUTPUT.mkdir(parents=True, exist_ok=True)
    lock = (OUTPUT/'runner.lock').open('a')
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    if (OUTPUT/'status.json').exists():
        raise RuntimeError('结果目录已有状态，不自动重跑；先核对已完成记录')
    rows = {t['task_id']: t for t in tasks()}
    known = dimensions(rows.values())
    pending = deque(plan['batches']); active = {}; results = {}; seen = set(); stop = []
    secrets = {v.encode() for k,v in env.items() if re.search(r'KEY|TOKEN|SECRET|PASSWORD',k,re.I) and len(v)>=16 and not v.isdigit()}
    pattern = re.compile(rb'\b(?:ark|sk)-[A-Za-z0-9_-]{20,}|Bearer\s+[A-Za-z0-9_./+\-=]{20,}',re.I)
    started = datetime.now(timezone.utc).isoformat()
    signal.signal(signal.SIGTERM, lambda *_: stop.append('requested_stop'))
    signal.signal(signal.SIGINT, lambda *_: stop.append('requested_stop'))
    save(OUTPUT/'plan.json', plan)
    save(OUTPUT/'runner.json', {'pid': os.getpid(), 'started_at': started, 'command': str(Path(__file__).resolve().relative_to(ROOT))})
    try:
        while pending or active:
            while pending and len(active) < 2 and not stop:
                batch = pending.popleft(); destination = OUTPUT/'batches'/batch['name']
                command = [str(BINARY), 'dataset', 'run', str(DATASET), '--config', batch['config'],
                           '--output-root', str(destination), '--allow-unreviewed']
                # 原始子进程 stdout 不复制到排程日志，结果以原生持久化报告为准。
                process = subprocess.Popen(command, cwd=ROOT, env=env, stdin=subprocess.DEVNULL,
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
                active[batch['name']] = (batch, process, destination)
                print('START', batch['name'], 'tasks', len(batch['task_ids']), flush=True)
            changed = False
            for name, (batch, process, destination) in list(active.items()):
                for path in sorted((destination/'trials').glob('*.json')):
                    if path in seen:
                        continue
                    try:
                        data = path.read_bytes(); trial = json.loads(data)
                    except (OSError, ValueError):
                        continue  # 原生写入尚未结束，下次只重读此文件。
                    if any(s in data for s in secrets) or pattern.search(data):
                        stop.append('privacy_scan_hit'); seen.add(path); continue
                    task_id = trial['task_id']
                    assert task_id in batch['task_ids'] and task_id not in results, 'trial_identity_mismatch'
                    result = inspect_trial(trial, rows[task_id]); result['report'] = str(path.relative_to(OUTPUT))
                    results[task_id] = result; seen.add(path); changed = True
                    print('DONE', f'{len(results)}/{plan["tasks"]}', task_id, result['verdict'],
                          'execution='+str(result['execution_status']),
                          'seconds='+str(round(result['duration_ms']/1000,1) if result['duration_ms'] is not None else 'unknown'), flush=True)
                code = process.poll()
                if code is not None:
                    report = destination/'report.json'
                    if code not in (0, 1, 3) or not report.exists() or not read(report).get('complete'):
                        stop.append('batch_incomplete:'+name)
                    del active[name]; changed = True
            completed = [rows[k] for k,v in results.items() if v['execution_status'] == 'completed' and not v['trace_issues']]
            coverage = dimensions(completed)
            if len(results) >= 4 and all(r['trace_issues'] or r['execution_status'] != 'completed' for r in list(results.values())[-4:]):
                stop.append('four_consecutive_execution_or_trace_errors')
            summary = {'started_at': started, 'updated_at': datetime.now(timezone.utc).isoformat(),
                'status': 'stopping' if stop and active else 'paused' if stop else 'running' if pending or active else 'completed',
                'planned_tasks': plan['tasks'], 'planned_turns': plan['turns'], 'completed_tasks': len(results),
                'execution_completed': sum(r['execution_status']=='completed' for r in results.values()),
                'trace_valid_tasks': len(completed), 'captured_turns': sum(r['captured_turns'] for r in results.values()),
                'verdicts': dict(Counter(r['verdict'] for r in results.values())),
                'by_split': {s: dict(Counter(r['verdict'] for r in results.values() if r['split']==s)) for s in ('train','val','holdout')},
                'coverage': {k: {'completed': len(coverage[k]), 'total': len(v)} for k,v in known.items()},
                'active_batches': list(active), 'pending_batches': len(pending), 'stop_reasons': sorted(set(stop)),
                'warning': '模型 passed 不等于无业务错误；相对日期漏判仍须复核。无入库/无进化。'}
            save(OUTPUT/'status.json', summary)
            if changed:
                save(OUTPUT/'results-index.json', list(results.values()))
            if stop:
                # 只取消本排程明确启动的评测器，让其正常保存已完成题目和释放子进程。
                for _, process, _ in active.values():
                    if process.poll() is None:
                        process.terminate()
                if not active:
                    break
            time.sleep(3)
    except Exception as error:
        # 仅记录异常类型，避免把任务内容或凭据拼入排程日志。
        save(OUTPUT/'status.json', {'status': 'paused', 'planned_tasks': plan['tasks'],
            'completed_tasks': len(results), 'stop_reasons': ['runner_'+type(error).__name__]})
        save(OUTPUT/'results-index.json', list(results.values()))
        raise
    finally:
        for _, process, _ in active.values():
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=30)
                except subprocess.TimeoutExpired:
                    # 此进程组由本排程独立创建，只终止明确归属的试次，不影响其他服务。
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait()
        lock.close()
    print('FINISHED', summary['status'], summary['completed_tasks'], summary['verdicts'], flush=True)


if __name__ == '__main__':
    os.umask(0o077)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--prepare', action='store_true', help='只准备冻结配置和覆盖计划，不调用模型')
    args = parser.parse_args()
    prepare() if args.prepare else run_business_coverage()
