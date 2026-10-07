#!/usr/bin/env python3
"""本地冷启动实验排程：复用真实业务入口和原生 CLI，不实现 Agent 或评分器。"""

import argparse
from collections import Counter
from copy import deepcopy
from datetime import datetime, timezone
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import signal
import shutil
import subprocess
import sys
import time

import run as coverage
import layout

ROOT = coverage.ROOT
CONFIG = coverage.CONFIG / 'coldstart-deployment'
OUTPUT = ROOT / 'trace_output/business-simulator/training'
DATASET = coverage.layout.TRAINING_DATASET if hasattr(coverage, 'layout') else Path('/tmp/business-simulator-dataset')
PLAN = CONFIG / 'plan.json'
BINARY = coverage.BINARY
read, save = coverage.read, coverage.save


# ``learn`` uses exit status 3 for a completed batch that contains an
# isolated run failure (and for a few other, genuinely incomplete states).
# The JSON status is therefore the recovery boundary.  Keep this predicate
# deliberately small and explicit: task-level ``run_outcomes.failed`` is
# retained as evidence, while queue/pipeline failures still stop the batch.
_LEARNING_TERMINAL_STATUSES = frozenset(('completed', 'completed_with_unresolved'))
_LEARNING_PENDING_FIELDS = (
    'pending_runs', 'unpublished_runs', 'training_pending_users',
    'memory_pending_reviews', 'agent_evaluation_pending_jobs',
    'shared_pending_users',
)


def learning_result_usable(learning, returncode):
    """Return whether a native learning result is safe to checkpoint.

    ``learn_cli`` returns 3 when a batch has task-level failures.  Such a
    batch is still usable when its pipeline reached a terminal status; the
    failed count remains in ``run_outcomes`` for reporting.  Empty/malformed
    JSON, pending queues, evaluator errors, and management failures are not
    completion and must be retried or surfaced to the caller.
    """
    if returncode not in (0, 3) or not isinstance(learning, dict) or not learning:
        return False
    if learning.get('shared_status') not in _LEARNING_TERMINAL_STATUSES:
        return False
    if learning.get('agent_evaluation_error'):
        return False
    outcomes = learning.get('run_outcomes')
    if not isinstance(outcomes, dict) or not outcomes:
        return False
    for count in outcomes.values():
        if isinstance(count, bool) or not isinstance(count, (int, float)) or count < 0:
            return False
    # Missing counters are treated as zero for backwards-compatible status
    # files, but any present non-numeric/negative counter is malformed.
    for name in _LEARNING_PENDING_FIELDS:
        value = learning.get(name, 0)
        if isinstance(value, bool) or not isinstance(value, (int, float)) or value < 0:
            return False
        if value:
            return False
    for name in ('training_failed_users', 'memory_management_failed_users',
                 'shared_quarantined_users'):
        value = learning.get(name, 0)
        if isinstance(value, bool) or not isinstance(value, (int, float)) or value < 0:
            return False
        if value:
            return False
    # A nested manager can expose pending work even when the aggregate field
    # was omitted by an older runtime.  Never treat that as completed.
    managers = learning.get('memory_management', {})
    if managers is not None:
        if not isinstance(managers, dict):
            return False
        for manager in managers.values():
            if not isinstance(manager, dict):
                return False
            status = manager.get('status')
            if status in ('pending', 'running', 'failed', 'waiting_for_model'):
                return False
            pending = manager.get('pending', 0)
            if isinstance(pending, bool) or not isinstance(pending, (int, float)) or pending < 0:
                return False
            if pending:
                return False
    return True


def environment():
    """凭据仅留在环境中，使用现有业务部署的 API 和模型。"""
    env = coverage.environment()
    env.setdefault('EVOLUTION_BUSINESS_TOKEN', secrets.token_urlsafe(32))
    env['TRAJECTORY_UNCONDITIONAL_RESUME'] = '1'
    return env


def prepare():
    """冻结完整 train；只在本轮副本使用用户确认过的原表验收标准。"""
    if OUTPUT.exists() or CONFIG.exists():
        if PLAN.is_file() and OUTPUT.is_dir():
            return
        raise RuntimeError('已有部分训练产物，请检查后续跑，不覆盖记忆')
    env = environment()
    rows = sorted((t for t in coverage.tasks() if t['split'] == 'train'),
                  key=lambda task: task['task_id'])
    if not rows or len({t['task_id'] for t in rows}) != len(rows):
        raise RuntimeError('训练集为空或任务身份重复')
    OUTPUT.mkdir(parents=True)
    CONFIG.mkdir(parents=True)
    if DATASET.exists():
        shutil.rmtree(DATASET)
    for task in rows:
        task['review_status'] = 'reviewed'
    source = CONFIG / 'training.jsonl'
    source.write_text(''.join(json.dumps(t, ensure_ascii=False)+'\n' for t in rows))
    builder = read(ROOT / 'tools/trajectory_evolution/config/dataset.json')
    builder.update(name='business-coldstart-full-training', sources=[{'kind': 'golden', 'path': 'training.jsonl'}])
    save(CONFIG / 'dataset.json', builder)
    subprocess.run([str(BINARY), 'dataset', 'build', '--config', str(CONFIG / 'dataset.json'),
                    '--output-root', str(DATASET)], env=env, check=True)
    from prepare_dataset import prepare_configuration
    deploy = prepare_configuration(CONFIG / 'runtime', DATASET)
    service = read(deploy / 'service.json')
    service.pop('policy_evaluation_file', None)
    service.pop('evaluation_levels', None)
    service.pop('control_learning', None)
    service.update(data_dir=str((OUTPUT / 'evolution').relative_to(ROOT)), max_level='L0', auto_evaluate=True,
                   workers=16)
    service['instrumentation']['level'] = 'L0'
    service['learning'].update(min_new_runs=4, min_interval_ms=0, max_batch_runs=32,
                               analysis_parallelism=16)
    service['learning'].update(max_analysis_retries=2, retry_backoff_ms=15000)
    service['sharing'].update(enabled=True, min_interval_ms=1000,
                              max_model_calls_per_cycle=32, model_parallelism=16)
    save(deploy / 'service.json', service)
    runtime = read(deploy / 'runtime.json')
    runtime.setdefault('evolution', {})['memory_review_parallelism'] = 16
    # 增量匹配仍在每批执行；全库语义复核只在较大批次触发，避免每约
    # 20 条新记忆重复扫描整个库；未决条目仍由活动 journal 触发增量复核。
    runtime['evolution']['full_curation_new_entry_threshold'] = 64
    save(deploy / 'runtime.json', runtime)
    base = read(deploy / 'evaluation.json')
    base.update(splits=['train'], repetitions=1, parallelism=4)
    # Judge semantic quality during training so memory admission has evidence.
    # The judge runs at the evaluation boundary and does not change AgentFlow
    # behavior; a failed judge remains task-scoped in the native evaluator.
    base['judge']['enabled'] = True
    base['judge']['batch_input_bytes'] = 2097152
    base['judge']['max_input_bytes'] = 2097152
    base['judge']['evidence_field_bytes'] = 256
    base['judge']['answer_field_bytes'] = 1024
    # Training keeps only the two canonical reports and a manifest.  Checkpoint
    # files are needed while a batch runs, then the evaluator removes them;
    # this keeps the full chain below the storage budget without changing
    # runtime scoring or recovery semantics.
    base['artifact_retention'] = {
        'retain_trial_files': False,
        'retain_markdown': False,
        'retain_jsonl': False,
    }
    # 首批覆盖多意图、多轮、提醒与 GUI；成功采集并学习后才继续其余场景。
    pilot_ids = ['TGJK_YD2_005', 'SYS_DYT_03_037', 'SYS_DYT_03_063']
    pilot = [t for name in pilot_ids for t in rows if t['task_id'] == name]
    groups = [pilot]
    remaining = [t for t in rows if t not in pilot]
    current, turns = [], 0
    for task in remaining:
        size = 1 + len(task.get('followup_inputs', []))
        if current and (len(current) >= 4 or turns + size > 12):
            groups.append(current)
            current, turns = [], 0
        current.append(task)
        turns += size
    if current:
        groups.append(current)
    batches = []
    for index, group in enumerate(groups, 1):
        name = f'{index:03d}'
        config = deepcopy(base)
        config.update(task_ids=[t['task_id'] for t in group], max_trials=len(group))
        path = deploy / f'training-{name}.json'
        save(path, config)
        batches.append(dict(name=name, config=str(path.relative_to(ROOT)), task_ids=config['task_ids']))
    plan = dict(mode='L0 training capture -> analysis -> meta reflection -> private/public memory',
                tasks=len(rows), turns=sum(1+len(t.get('followup_inputs', [])) for t in rows),
                coverage={key: len(value) for key, value in coverage.dimensions(rows).items()},
                dataset_files=read(DATASET / 'manifest.json')['files'], batches=batches,
                service=str((deploy / 'service.json').relative_to(ROOT)), task_judge=True,
                trajectory_model_analysis=True, meta_reflection=True, shared_management=True,
                original_dataset_unchanged=True, reviewed_for_this_experiment=True)
    save(PLAN, plan)
    print(json.dumps({key: plan[key] for key in ('tasks', 'turns', 'coverage')}, ensure_ascii=False))


def tune_parallelism(parallelism):
    """只重排未启动的题；保留完成批次、冻结数据集与全部既有记忆。"""
    with (OUTPUT / 'runner.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        state = read(OUTPUT / 'status.json')
        if state['status'] == 'running':
            raise RuntimeError('runner_must_be_paused')
        plan = read(PLAN)
        completed = [batch for batch in plan['batches'] if batch['name'] in state['learned']]
        remaining = [batch for batch in plan['batches'] if batch['name'] not in state['learned']]
        if plan['batches'] != completed + remaining or not remaining:
            raise RuntimeError('only_unstarted_suffix_can_be_rebatched')
        if any((OUTPUT / batch['name']).exists() for batch in remaining):
            raise RuntimeError('finish_or_repair_captured_batch_before_rebatching')
        task_ids = [task for batch in remaining for task in batch['task_ids']]
        task_map = {task['task_id']: task for task in coverage.tasks()}
        template = read(ROOT / remaining[0]['config'])
        groups, current, turns = [], [], 0
        turn_limit = max(32, parallelism * 4)
        for task_id in task_ids:
            size = 1 + len(task_map[task_id].get('followup_inputs', []))
            if current and (len(current) >= parallelism or turns + size > turn_limit):
                groups.append(current)
                current, turns = [], 0
            current.append(task_id)
            turns += size
        if current:
            groups.append(current)
        batches = []
        for index, group in enumerate(groups, len(completed) + 1):
            name = f'{index:03d}'
            config = deepcopy(template)
            config.update(task_ids=group, max_trials=len(group), parallelism=parallelism)
            path = CONFIG / 'runtime' / f'training-{name}.json'
            save(path, config)
            batches.append(dict(name=name, config=str(path.relative_to(ROOT)), task_ids=group))
        assert task_ids == [task for batch in batches for task in batch['task_ids']]
        previous_count = len(plan['batches'])
        plan.update(batches=completed + batches, concurrency=parallelism, max_batch_turns=turn_limit,
                    concurrency_changed_at=datetime.now(timezone.utc).isoformat())
        save(PLAN, plan)
        # 只移除旧计划明确列出的、重排后不再引用的生成配置，不碰任何执行结果。
        retained = {batch['config'] for batch in plan['batches']}
        for batch in remaining:
            path = ROOT / batch['config']
            if batch['config'] not in retained and path.parent == CONFIG / 'runtime':
                path.unlink()
        print(json.dumps(dict(parallelism=parallelism, completed_batches=len(completed),
                              remaining_tasks=len(task_ids), previous_batches=previous_count,
                              batches=len(plan['batches'])), ensure_ascii=False))


def inspect(report, task_map, env):
    """检查每轮轨迹和凭据泄露；单题执行失败只记录，不阻断整批。"""
    raw = report.read_bytes()
    secret_values = {v.encode() for k, v in env.items()
                     if re.search(r'KEY|TOKEN|SECRET|PASSWORD', k, re.I) and len(v) >= 16 and not v.isdigit()}
    if any(value in raw for value in secret_values):
        raise RuntimeError('privacy_scan_hit')
    if re.search(rb'\b(?:ark|sk)-[A-Za-z0-9_-]{20,}|Bearer\s+[A-Za-z0-9_./+\-=]{20,}', raw, re.I):
        raise RuntimeError('credential_pattern_hit')
    value = json.loads(raw)
    if not value.get('complete'):
        raise RuntimeError('training_report_incomplete')
    rows = [coverage.inspect_trial(t, task_map[t['task_id']]) for t in value['trials']]
    failures = [r for r in rows if r['trace_issues'] or
                r['execution_status'] != 'completed' or r['error_code']]
    # A timeout, 429, invalid model response, or judge error is task scoped.
    # Keep it in the batch summary and let the native importer learn from the
    # completed trials.  Stop only when no usable trial remains or the report
    # itself is structurally unsafe.
    usable = [r for r in rows if not r['trace_issues'] and
              r['execution_status'] == 'completed' and
              r['error_code'] in (None, '', 'evaluation_judge_failed')]
    if not usable:
        raise RuntimeError('batch_no_usable_trials')
    if failures:
        print('TASK_FAILURES', json.dumps({
            'count': len(failures),
            'task_ids': [r['task_id'] for r in failures],
            'error_codes': sorted({r['error_code'] for r in failures if r['error_code']}),
        }, ensure_ascii=False), flush=True)
    return rows


def capture_needs_resume(report):
    """整批取消也要续跑，即使所有单题恰好已完成；由原生 CLI 校验并复用检查点。"""
    return not report.get('complete') or any(
        trial['execution_status'] not in ('completed', 'failed') or trial.get('error_code')
        for trial in report['trials'])


def report_capture_progress(destination, seen, finished=False):
    """运行中容忍旧版本的半写文件；进程结束后仍损坏则明确报错，不静默漏报。"""
    for path in sorted((destination / 'trials').glob('*.json')):
        if path in seen:
            continue
        try:
            trial = read(path)
        except (FileNotFoundError, json.JSONDecodeError, UnicodeDecodeError):
            if finished:
                raise RuntimeError(f'trial_checkpoint_invalid:{path.name}') from None
            continue
        print('TASK', trial['task_id'], trial['execution_status'], trial['verdict'],
              'seconds='+str(round(trial.get('duration_ms', 0)/1000, 1)), flush=True)
        seen.add(path)


def run(retry_failed=False, stop_after_batch=None):
    """逐批真实执行并学习；成功报告和已入库批次可复用，异常时停止等待修复。"""
    plan = read(PLAN)
    env = environment()
    for name, expected in plan['dataset_files'].items():
        if hashlib.sha256((DATASET / name).read_bytes()).hexdigest() != expected:
            raise RuntimeError('frozen_dataset_changed')
    lock = (OUTPUT / 'runner.lock').open('a')
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    task_map = {t['task_id']: t for t in coverage.tasks()}
    state = read(OUTPUT / 'status.json') if (OUTPUT / 'status.json').exists() else dict(results=[], learned=[])
    state.update(status='running', pid=os.getpid(), planned_tasks=plan['tasks'], planned_turns=plan['turns'])
    # 本次已恢复运行，历史暂停原因保留在日志中，不再混入当前状态。
    state.pop('error', None)
    stopped = []
    signal.signal(signal.SIGTERM, lambda *_: stopped.append('requested_stop'))
    signal.signal(signal.SIGINT, lambda *_: stopped.append('requested_stop'))
    child = None
    try:
        for batch in plan['batches']:
            if batch['name'] in state['learned']:
                continue
            destination = OUTPUT / batch['name']
            report = destination / 'report.json'
            state.update(batch=batch['name'], phase='capture', updated_at=datetime.now(timezone.utc).isoformat())
            save(OUTPUT / 'status.json', state)
            resume = []
            if retry_failed and report.exists() and capture_needs_resume(read(report)):
                # 原报告只移动不覆盖；原生续跑保留已完成试次的身份和轨迹。
                attempt = 1
                previous = destination.with_name(destination.name + f'-attempt-{attempt:02d}')
                while previous.exists():
                    attempt += 1
                    previous = destination.with_name(destination.name + f'-attempt-{attempt:02d}')
                destination.rename(previous)
                resume = ['--resume-report', str(previous / 'report.json')]
            if not report.exists():
                if destination.exists():
                    raise RuntimeError('incomplete_capture_needs_review')
                print('CAPTURE', batch['name'], ','.join(batch['task_ids']), flush=True)
                child = subprocess.Popen([str(BINARY), 'dataset', 'run', str(DATASET), '--config', batch['config'],
                    '--output-root', str(destination), *resume], cwd=ROOT, env=env, stdin=subprocess.DEVNULL,
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
                seen = set()
                while child.poll() is None:
                    report_capture_progress(destination, seen)
                    if stopped:
                        child.terminate()
                    time.sleep(2)
                if child.returncode not in (0, 1, 3) or not report.exists():
                    raise RuntimeError('capture_failed')
                # 退出前刚写完的最后几题也要检查和播报；最终完成状态仍以完整报告为准。
                report_capture_progress(destination, seen, finished=True)
            rows = inspect(report, task_map, env)
            state['results'] = [r for r in state['results'] if r['task_id'] not in batch['task_ids']] + rows
            state.update(phase='learning', captured_tasks=len(state['results']),
                         captured_turns=sum(r['captured_turns'] for r in state['results']))
            save(OUTPUT / 'status.json', state)
            print('LEARN', batch['name'], 'runs='+str(sum(r['captured_turns'] for r in rows)), flush=True)
            child = subprocess.Popen([str(BINARY), 'learn', '--config', plan['service'],
                    '--training-evaluation', str(destination), '--dataset', str(DATASET)], cwd=ROOT, env=env,
                    stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE, start_new_session=True)
            # communicate 持续排空管道，避免输出填满后父子进程互相等待。
            while True:
                try:
                    out, err = child.communicate(timeout=2)
                    break
                except subprocess.TimeoutExpired:
                    if stopped:
                        child.terminate()
            try:
                learning = json.loads(out)
                save(destination / 'learning-status.json', learning)
                state['learning'] = learning
            except (ValueError, TypeError):
                raise RuntimeError('learning_status_missing')
            # The native learner can return non-zero after completing the
            # batch (for example when one task-level run failed).  The JSON
            # status is the authoritative recovery boundary: only reject a
            # batch when it is missing, explicitly failed, or structurally
            # unusable.  This keeps isolated model/judge failures from
            # replaying an otherwise valid batch three times.
            learning_ok = learning_result_usable(learning, child.returncode)
            # The JSON status and the native return code are one contract:
            # rc=0 is not sufficient when the status is pending/malformed.
            # Reject every non-usable result so a partially finished learn
            # cannot be checkpointed as a completed batch.
            if not learning_ok:
                raise RuntimeError('learning_failed')
            state['learned'].append(batch['name'])
            state['updated_at'] = datetime.now(timezone.utc).isoformat()
            save(OUTPUT / 'status.json', state)
            print('LEARNED', batch['name'], 'private_entries='+str(learning.get('private_entries', 'see_memory')),
                  'shared_status='+str(learning.get('shared_status')), flush=True)
            # 容量测试等维护操作只在完整批次落盘后执行，不中断本轮模型请求。
            if stop_after_batch is not None and int(batch['name']) >= stop_after_batch:
                state.update(status='paused', phase='batch_completed', error='requested_batch_pause')
                break
            if stopped:
                raise RuntimeError('requested_stop')
        else:
            state.update(status='completed', phase='completed')
    except Exception as exc:
        # A requested stop is a checkpoint boundary, not a model/runner
        # failure.  Preserve that distinction so the outer recovery loop does
        # not immediately replay the same batch after a deliberate pause.
        error = 'requested_stop' if stopped else (
            str(exc) if type(exc) is RuntimeError else type(exc).__name__)
        state.update(status='paused', error=error)
        print('PAUSED', state['error'], flush=True)
    finally:
        if child is not None and child.poll() is None:
            child.terminate()
            try:
                child.wait(timeout=30)
            except subprocess.TimeoutExpired:
                os.killpg(child.pid, signal.SIGKILL)
                child.wait()
        state['updated_at'] = datetime.now(timezone.utc).isoformat()
        save(OUTPUT / 'status.json', state)
        lock.close()
    return 0 if state['status'] == 'completed' else 1


def compact_completed_outputs():
    """Drop regenerable training copies after the full training stage succeeds."""
    state_file = OUTPUT / 'status.json'
    state = read(state_file)
    if state.get('status') != 'completed' or state.get('captured_tasks') != len((coverage.CONFIG / 'datasets/pack/train.jsonl').read_text().splitlines()):
        raise RuntimeError('training_not_complete_for_compaction')
    removed_bytes = 0
    removed_files = 0

    def remove(path):
        nonlocal removed_bytes, removed_files
        if not path.exists() or path.is_symlink():
            return
        if path.is_file():
            removed_bytes += path.stat().st_size
            removed_files += 1
            path.unlink()
        elif path.is_dir():
            for child in path.rglob('*'):
                if child.is_file():
                    removed_bytes += child.stat().st_size
                    removed_files += 1
            import shutil
            shutil.rmtree(path)

    # Reports and manifests are canonical evidence.  Trial JSON, JSONL and
    # Markdown are exact duplicates and can be regenerated from report.json.
    for batch in sorted(OUTPUT.glob('[0-9][0-9][0-9]')):
        remove(batch / 'trials')
        remove(batch / 'report.md')
        remove(batch / 'results.jsonl')
    # Raw traces and scheduler batches have already been summarized into the
    # memory stores and status.json.  Keep the memory files used by GEPA.
    evolution = OUTPUT / 'evolution' / 'by-user' / 'business-training'
    for name in ('traces', 'evaluation', 'batches'):
        remove(evolution / name)
    summary = {
        'status': 'compacted',
        'canonical_reports': len(list(OUTPUT.glob('[0-9][0-9][0-9]/report.json'))),
        'removed_files': removed_files,
        'removed_bytes': removed_bytes,
    }
    save(OUTPUT / 'compaction.json', summary)
    print('COMPACTED', json.dumps(summary, ensure_ascii=False), flush=True)
    return summary


if __name__ == '__main__':
    os.umask(0o077)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--prepare', action='store_true')
    parser.add_argument('--retry-failed', action='store_true')
    parser.add_argument('--compact', action='store_true')
    parser.add_argument('--stop-after-batch', type=int)
    parser.add_argument('--tune-parallelism', type=int, choices=range(1, 17))
    args = parser.parse_args()
    if args.compact:
        sys.exit(0 if compact_completed_outputs() else 1)
    if args.tune_parallelism:
        tune_parallelism(args.tune_parallelism)
    else:
        sys.exit(prepare() if args.prepare else run(args.retry_failed, args.stop_after_batch))
