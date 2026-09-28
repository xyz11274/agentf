#!/usr/bin/env python3
"""真实 v1 业务评测：固定环境、逐轮证据、独立意图观察。"""
from __future__ import annotations

import hashlib
import json
import os
import shutil
import sys
import uuid
import argparse
import tempfile
from pathlib import Path

PACKAGE = Path(__file__).resolve().parents[1]
REPO = PACKAGE.parents[2]
sys.path.insert(0, str(REPO / 'benchmark'))
sys.path.insert(0, str(PACKAGE / 'harness'))
import run_eval as benchmark
from converters import read_xlsx_rows
from business_sim.state import Store, fresh_state, next_id, scope_key


_DAEMON_START_ATTEMPTS = 3
_DAEMON_START_TIMEOUT_SECONDS = 5.0


def _unused_loopback_port(excluded: set[int]) -> int:
    """返回本试次尚未尝试过的端口；关闭保留 socket 后必须再验证实际归属。"""
    import socket
    for _ in range(32):
        with socket.socket() as sock:
            sock.bind(('127.0.0.1', 0))
            port = sock.getsockname()[1]
        if port not in excluded:
            return port
    raise RuntimeError('business_daemon_port_allocation_failed')


def _stop_daemon(process, timeout: float = 2.0) -> None:
    """终止本次 daemon 进程组并 wait 回收直接子进程，失败重试不得遗留 zombie。"""
    import signal
    import subprocess
    if process is None:
        return
    pid = process.pid
    if process.poll() is None:
        try:
            os.killpg(pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        try:
            process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait()
    else:
        # poll() 通常已经 waitpid；显式 wait() 保持所有 Popen 实现的回收约定一致。
        process.wait()
    # 主进程先退出时仍可能有同一私有进程组的后代；不能让它们污染下一次尝试。
    try:
        os.killpg(pid, signal.SIGKILL)
    except ProcessLookupError:
        pass


def _start_business_daemon(base_config: dict, config_path: Path, sockets: Path, log,
                           env: dict, *, package: Path = PACKAGE,
                           attempts: int = _DAEMON_START_ATTEMPTS,
                           attempt_timeout: float = _DAEMON_START_TIMEOUT_SECONDS):
    """用新端口有界启动 daemon，并以成功写入的 PID 文件校验监听端口归属。"""
    import subprocess
    import time
    if attempts < 1 or attempt_timeout <= 0:
        raise ValueError('invalid business daemon startup budget')
    used_ports: set[int] = set()
    last_error = 'not_started'
    for attempt in range(1, attempts + 1):
        port = _unused_loopback_port(used_ports)
        used_ports.add(port)
        pid_file = sockets/f'daemon-{attempt}.pid'
        config = json.loads(json.dumps(base_config))
        config['server'].update(listen=f'127.0.0.1:{port}',
            uds_biz_path=str(sockets/f'biz-{attempt}.sock'),
            uds_ctrl_path=str(sockets/f'ctrl-{attempt}.sock'))
        config['daemon']['pid_file'] = str(pid_file)
        config_path.write_text(json.dumps(config), encoding='utf-8')
        process = None
        try:
            process = subprocess.Popen([
                env.get('AGENTFLOW_DAEMON', str(REPO/'build/agentflowd')),
                '--config', str(config_path), '--package', str(package),
            ], cwd=REPO, env=env, stdout=log, stderr=subprocess.STDOUT,
                start_new_session=True)
            deadline = time.monotonic() + attempt_timeout
            while time.monotonic() < deadline:
                exit_code = process.poll()
                if exit_code is not None:
                    process.wait()
                    last_error = f'exited:{exit_code}'
                    break
                client = benchmark.RpcClient('127.0.0.1', port, timeout=.5)
                try:
                    benchmark.check_daemon(client, 'business-simulator')
                    # 相邻试次可能抢到 bind 后释放的端口。只有本 Popen 在 RPC 启动成功后
                    # 写出的 PID 文件与进程一致，才允许把该监听器当成本试次 daemon。
                    owner = int(pid_file.read_text(encoding='utf-8').strip())
                    if owner == process.pid and process.poll() is None:
                        return process, port
                    last_error = 'listener_owner_mismatch'
                except (OSError, RuntimeError, ValueError):
                    last_error = 'port_not_listening'
                finally:
                    client.close()
                if process.poll() is not None:
                    process.wait()
                    last_error = f'exited:{process.returncode}'
                    break
                time.sleep(.05)
            else:
                last_error = 'port_not_listening'
        except OSError:
            last_error = 'spawn_failed'
        _stop_daemon(process)
    raise RuntimeError(f'business_daemon_start_failed:{last_error}') from None


def _prepare_trial_package(workspace: Path) -> Path:
    """Create a tiny per-trial package and bind workflows to that package.

    Workflow ``working_dir`` values are generated deployment details and may
    point at a temporary package that no longer exists.  A native evaluation
    trial must never depend on that stale path. Most static package children
    are symlinked, while node launchers, workflows, and mutable output roots
    are private so artifacts and SQLite state cannot escape the trial.
    """
    package = workspace / 'agent-package'
    if package.exists() or package.is_symlink():
        raise RuntimeError('business_trial_package_already_exists')
    package.mkdir(parents=True, exist_ok=False)
    workflows = {
        'coordinator_workflow.json',
        'dummy_pre_skill_workflow.json',
        'skill_workflow.json',
    }
    for child in PACKAGE.iterdir():
        destination = package / child.name
        if child.name in workflows:
            continue
        if child.name == 'nodes':
            shutil.copytree(child, destination, symlinks=False)
            continue
        if child.name == 'outputs':
            destination.mkdir()
            # Browser binaries are immutable runtime resources; expose only
            # these trees and keep session artifacts/DB private.
            for name in ('browser-runtime', 'browser-libs', 'browsers'):
                source = child / name
                if source.exists():
                    (destination / name).symlink_to(source, target_is_directory=True)
            continue
        if child.name == 'data':
            shutil.copytree(child, destination, symlinks=False)
            continue
        if child.name.startswith('.env'):
            continue
        destination.symlink_to(child, target_is_directory=child.is_dir())
    for name in workflows:
        source = PACKAGE / name
        document = json.loads(source.read_text(encoding='utf-8'))
        for node in document.get('nodes', []):
            if isinstance(node, dict) and 'working_dir' in node:
                node['working_dir'] = str(package)
        (package / name).write_text(
            json.dumps(document, ensure_ascii=False, separators=(',', ':')) + '\n',
            encoding='utf-8',
        )
    return package


def load_preconditions() -> dict:
    result = {}
    for path in sorted((REPO / 'test_cases/product').glob('*.xlsx')):
        rows = read_xlsx_rows(path)
        for row in rows[1:]:
            fields = dict(zip(rows[0], row))
            result[(path.name, fields.get('用例编号'))] = {
                'text': fields.get('预置条件（可选）', '').strip(),
                'device_type': fields.get('设备类型', ''),
            }
    return result


def seed_case(case: dict, user: str, preconditions: dict, store: Store, profile=None) -> tuple[str, dict]:
    provenance = case.get('provenance', {})
    source = Path(provenance.get('source', '')).name
    original_id = provenance.get('original_case_id', case['id'])
    row = preconditions.get((source, original_id), {})
    text = row.get('text', provenance.get('precondition', ''))
    state = fresh_state()
    evidence = {'precondition': text, 'seeded': []}
    if profile is not None:
        from business_sim.evaluation_world import seed
        seed(state, profile)
        evidence.update(snapshot_sha256=profile['sha256'], clock=profile['clock'],
                        history_days=profile.get('history_days', 0))
    if '用户喜好科幻' in text:
        state['preferences']['电影偏好'] = '科幻'
        evidence['seeded'].append('电影偏好=科幻')
    elif '用户喜好动画' in text:
        state['preferences']['电影偏好'] = '动画'
        evidence['seeded'].append('电影偏好=动画')
    if '已有健康档案及医嘱用药记录' in text:
        rid = next_id(state, 'record')
        state['records'][rid] = {'id': rid, 'origin': 'synthetic_precondition',
            'content': '模拟医嘱记录：维生素D，每天08:00；剂量未提供，不能自行补全。'}
        evidence['seeded'].append('synthetic medical record; no real patient data')
    supported = ('', '无', '已注入声纹 B，用户喜好科幻', '已注入声纹 B，用户喜好动画', '已有健康档案及医嘱用药记录')
    if text not in supported:
        raise ValueError(f"Unimplemented precondition for {case['id']}: {text}")
    target = {'1': 'phone-1', '2': 'speaker-1', '3': 'tv-1'}.get(row.get('device_type'))
    if target:
        if profile is not None:
            target = 'virtual-' + target
        state['preferences']['target_device'] = target
        evidence['seeded'].append(f'target_device={target}')
    client = f"eval-{case['id'].lower()[:32]}"  # benchmark.run_case identity contract
    scope = scope_key(user, client)
    store.reset(scope, state)
    return scope, evidence


def run_evolution_entry():
    """用原 daemon/RPC 执行组件派送的输入；采集、评分、训练归组件负责。"""
    import time
    from business_sim.backend import BusinessServer
    # 这是业务部署入口，不属于进化包；包内不认识 AgentFlow、Excel 或业务工具。
    sys.path.insert(0, str(REPO/'tools/trajectory_evolution/clients/python'))
    from evolution_entry import evaluate
    if os.environ.get('TRAJECTORY_EVOLUTION_EVALUATION_ENTRY') != '1' or not os.environ.get('TRAJECTORY_EVOLUTION_TRIAL_INITIAL_STATE'):
        raise ValueError('此入口仅由原生 dataset run / GEPA 的隔离试次启动')
    # 启动前验证初态版本，并提前准备只读环境事实，绝不读取验收标准。
    state = fresh_state()
    facts = (f"当前为业务仿真环境，业务时间固定为 {state['evaluation_world']['clock']}。"
             "天气、片库、产品资料以测试工具回执为准，非实时外部业务。")
    config_path = Path(os.environ['TRAJECTORY_EVOLUTION_TRIAL_CONFIG']).expanduser().resolve(strict=True)
    workspace = config_path.parent
    trial_package = _prepare_trial_package(workspace)
    database = workspace / 'business.sqlite3'
    backend = BusinessServer(('127.0.0.1', 0), database=database).start()
    process = None
    # UNIX socket 需要短路径；临时目录随试次关闭，不在仓库根留下实验目录。
    try:
        with tempfile.TemporaryDirectory(prefix='af-business-entry-') as sockets, (workspace/'daemon.log').open('w') as log:
            config = json.loads((PACKAGE/'daemon.json').read_text())
            config['trace'] = {'output_dir': str(workspace/'native-traces')}
            daemon_config_path = workspace/'daemon.json'
            # 离线冷启动允许等待本地采集确认；不修改线上客户端的 20ms 默认预算。
            point_timeout = int(os.environ.get('BUSINESS_EVALUATION_POINT_TIMEOUT_MS', '5000'))
            if not 1 <= point_timeout <= 5000:
                raise ValueError('BUSINESS_EVALUATION_POINT_TIMEOUT_MS must be in [1, 5000]')
            point_config = workspace/'point-client.json'
            # 两者都是 DataBus 派发/回收关联键，不是凭据；只声明公开标识，保留其他密钥检查。
            point_config.write_text(json.dumps({'point_timeout_ms': point_timeout,
                'privacy': {'public_env_names': ['AF_EXEC_SKILL_KEY', 'AF_COMPLETED_BATCH_KEY']}}), encoding='utf-8')
            env = {**os.environ, 'AF_SKIP_DOTENV': '1', 'AF_COORDINATOR_VERSION': 'v1',
                   'AF_AGENT_PACKAGE_DIR': str(trial_package.resolve()),
                   'BUSINESS_SIM_DB': str(database.resolve()),
                   'PYTHONDONTWRITEBYTECODE': '1',
                   'EVOLUTION_POINT_CONFIG': str(point_config),
                   'AF_SESSIONLOG_ENABLED': '0', 'AF_TRACE_OUTPUT_DIR': str(workspace/'native-traces'),
                   'BUSINESS_BACKEND_URL': backend.url, 'BUSINESS_EXECUTION_MODE': 'real',
                   'PYTHON_PATH': str(Path(sys.executable).parent.parent),
                   'PATH': str(Path(sys.executable).parent)+os.pathsep+os.environ.get('PATH', ''),
                   'PYTHONPATH': os.pathsep.join([str(trial_package/'harness'),
                       str(REPO/'tools/trajectory_evolution/clients/python'),
                       str(REPO/'packages/agentflow-runtime/src'), os.environ.get('PYTHONPATH', '')])}
            process, port = _start_business_daemon(
                config, daemon_config_path, Path(sockets), log, env, package=trial_package)

            def receive(task):
                # 组件提供真实历史和逐轮运行标识；只通过原来的 RPC 输入消息。
                if process.poll() is not None:
                    process.wait()
                    raise RuntimeError('business_daemon_exited')
                client = benchmark.RpcClient('127.0.0.1', port, timeout=5)
                try:
                    messages = [{'role': 'system', 'content': facts}, *task['messages']]
                    benchmark.send_turn(client, 'business-simulator', task['user_id'], task['conversation_id'],
                        task['run_key'], json.dumps({'stream': False, 'messages': messages}, ensure_ascii=False))
                    response = benchmark.wait_turn_response(client, time.time() + task['budget']['timeout_ms']/1000)
                    if response.get('timed_out') or response.get('error'):
                        raise RuntimeError('business_turn_delivery_failed')
                finally:
                    client.close()

            try:
                if not evaluate(receive):
                    raise RuntimeError('business_evaluation_entry_not_enabled')
            finally:
                _stop_daemon(process, timeout=10)
                process = None
    finally:
        _stop_daemon(process, timeout=10)
        backend.close()
    return 0


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    if argv == ['--evolution-entry']:
        return run_evolution_entry()
    options = argparse.ArgumentParser(add_help=False)
    options.add_argument('--world-profile', default=str(PACKAGE/'data/evaluation-profile.json'))
    options.add_argument('--live-world', action='store_true', help='使用实时环境，不启用固定快照')
    extra, argv = options.parse_known_args(argv)
    if not any(a == '--agent-id' or a.startswith('--agent-id=') for a in argv):
        argv += ['--agent-id', 'business-simulator']
    if not any(a == '--port' or a.startswith('--port=') for a in argv):
        argv += ['--port', '17893']
    if not any(a == '--evalset' or a.startswith('--evalset=') for a in argv):
        # 每次从原表转换，避免悄悄复用修复前拆错的 product.json。
        from converters import convert_product, merge_evalsets, load_domain_map
        mapping = load_domain_map(REPO/'benchmark/domain_skill_map.json')
        warnings = []
        dataset = merge_evalsets([convert_product(p, mapping, warnings)
                                 for p in sorted((REPO/'test_cases/product').glob('*.xlsx'))])
        destination = PACKAGE/'outputs/product-current.json'
        destination.parent.mkdir(parents=True, exist_ok=True)
        # 多个隔离 worker 同时启动时，读取方也只能看到一份完整的数据集。
        with tempfile.NamedTemporaryFile(mode='w', encoding='utf-8', dir=destination.parent, delete=False) as stream:
            json.dump(dataset, stream, ensure_ascii=False, indent=2)
            temporary = Path(stream.name)
        temporary.replace(destination)
        argv += ['--evalset', str(destination)]
        for warning in warnings:
            print('[dataset] '+warning, file=sys.stderr)
    args = benchmark.parse_args(argv)
    if args.concurrency != 1:
        raise ValueError('This package requires serial cases per daemon; use evals/run_isolated.py for parallel isolated daemons')
    if args.agent_id != 'business-simulator':
        raise ValueError('This fixture adapter targets business-simulator only; use benchmark/run_eval.py for other agents')
    if args.env_file:
        benchmark.load_env(Path(args.env_file))
    os.environ.setdefault("AF_TRACE_OUTPUT_DIR", str(REPO / "trace_output/business-simulator"))
    preconditions = load_preconditions()
    from business_sim.evaluation_world import load_profile
    from business_observer import BusinessObserver, multi_intent_metrics
    profile = None if extra.live_world else load_profile(extra.world_profile)
    run_id = uuid.uuid4().hex[:10]
    original = benchmark.run_case
    original_send, original_wait = benchmark.send_turn, benchmark.wait_turn_response
    original_metrics = benchmark.compute_metrics
    from conversation_client import send_turn, wait_turn_response

    def run_with_fixture(case, cfg):
        # Fresh identities also isolate the runtime's dialogue history across repeated runs.
        case_hash = hashlib.sha256(case['id'].encode()).hexdigest()[:12]
        cfg = {**cfg, 'user_id': f"{cfg['user_id'][:50]}-{run_id}-{case_hash}"}
        store = Store()
        scope, evidence = seed_case(case, cfg['user_id'], preconditions, store, profile)
        result = original(case, {**cfg, 'intent_observation_only': True,
                                 'conversation_context': (f"当前为业务仿真环境，业务时间固定为 {profile['clock']}。"
                                    "天气、片库、产品资料以测试工具回执为准，非实时外部业务。" if profile else ''),
                                 'turn_observer': BusinessObserver(store, scope)})
        state = store.read(scope)
        # 报告保留快照哈希和证据，不再复制整个片库、知识库及天气配置。
        state.pop('evaluation_world', None)
        result['business_execution'] = {**evidence, 'scope': scope, 'mode': os.environ.get('BUSINESS_EXECUTION_MODE','real'),
            'state': state, 'events': store.events(scope),
            'note': '业务结果按逐轮工具证据评分；意图仅观察，不进入任务分数'}
        return result

    # 仅此入口启用业务证据评分；公共 runner 默认行为与原始标签不变。
    benchmark.run_case = run_with_fixture
    def metrics(cases):
        report = original_metrics(cases)
        report['multi_intent_observation'] = multi_intent_metrics(cases)
        return report
    benchmark.compute_metrics = metrics
    benchmark.send_turn, benchmark.wait_turn_response = send_turn, wait_turn_response
    try:
        return benchmark.main(argv)
    finally:
        benchmark.run_case = original
        benchmark.compute_metrics = original_metrics
        benchmark.send_turn, benchmark.wait_turn_response = original_send, original_wait


if __name__ == '__main__':
    raise SystemExit(main())
