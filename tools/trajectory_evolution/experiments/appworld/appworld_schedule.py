"""Run a fixed AppWorld train/dev experiment without repeating cached trials."""
from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor
import fcntl
from pathlib import Path
import time

_CAPTURE_PROOF = {}


def _healthy(controller, report, expected_version):
    rows = report.get('trials', [])
    return (report.get('complete') is True and len(rows) == 1
            and rows[0].get('execution_status') == 'completed'
            and rows[0].get('evidence', {}).get('loaded_version') == expected_version
            and controller.metrics(rows)['scored'] == 1
            and controller.metrics(rows)['errored'] == 0)


def _health(controller, label, external, env):
    expected_version = controller.version(controller.read(controller.PUBLISHED / 'L0.json'))
    if external is not None:
        external = Path(external)
        while True:
            try:
                if external.is_file() and _healthy(controller, controller.read(external), expected_version):
                    return external
            except (OSError, ValueError):
                pass  # The owning health runner may be replacing the report.
            controller.state(status='running', phase='waiting_for_fixed_native_health',
                             expected_report=str(external), health_probe=label)
            time.sleep(5)
    report, path = controller.capture(label, controller.RUNTIME / (label + '.json'), env)
    if not _healthy(controller, report, expected_version):
        controller.state(status='blocked_by_host_error', phase=label, report_path=str(path))
        return None
    return path


def _business_agents_released(controller):
    """Task capture completion releases Agent slots before judge-only work ends."""
    base = controller.ROOT / 'trace_output/business-simulator/published/seed004/training-200'
    status_path = base / 'status.json'
    try:
        status = controller.read(status_path) if status_path.is_file() else {}
    except (OSError, ValueError):
        return False
    # Completion metadata may still describe a preceding recovery attempt.
    # Check only native dataset-capture processes for this exact experiment.
    for entry in Path('/proc').iterdir():
        if not entry.name.isdigit():
            continue
        try:
            command = (entry / 'cmdline').read_bytes()
        except (OSError, PermissionError):
            continue
        if (b'trajectory_evolution\x00dataset\x00run\x00' in command
                and str(base).encode() in command):
            return False
    if status.get('phase') == 'full' and status.get('status') in ('complete', 'incomplete'):
        return True
    # Consult only the current attempt, and parse a large completed report once.
    filename = status.get('report_path')
    if filename:
        report = Path(filename)
        if not report.is_absolute():
            report = controller.ROOT / report
        if report.is_file():
            try:
                stamp = report.stat().st_mtime_ns
                cached = _CAPTURE_PROOF.get(str(report))
                if cached is None or cached[0] != stamp:
                    cached = (stamp, controller.read(report).get('complete') is True)
                    _CAPTURE_PROOF[str(report)] = cached
                if cached[1]:
                    return True
            except (OSError, ValueError):
                pass
    return False


def run(controller, preflight_train=None, preflight_dev=None):
    controller.TRAINING.mkdir(parents=True, exist_ok=True)
    env = controller.environment()
    with (controller.TRAINING / 'controller.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        existing = controller.RUNTIME / 'plan.json'
        plan = controller.read(existing) if existing.is_file() else None
        if not plan or plan.get('host_version') != controller.HOST_VERSION:
            plan = controller.prepare()
        # Resume uses the frozen runtime, including any independently repaired
        # learning transport settings; it must not recreate defaults here.
        controller.state(status='running', phase='native_health')
        probes = {}
        for label, external in (('train-preflight', preflight_train), ('dev-preflight', preflight_dev)):
            path = _health(controller, label, external, env)
            if path is None:
                return
            probes[label] = path
        # Business already owns the requested sixteen slots. Start the large
        # AppWorld batch immediately when its captures complete; judge repair
        # and report export do not delay the AppWorld Agent experiment.
        while not _business_agents_released(controller):
            controller.state(status='running', phase='waiting_for_business_agent_slots',
                             native_health='healthy', requested_open_agent_parallelism=16)
            time.sleep(5)
        controller.state(phase='training_and_cached_baseline',
                         parallelism={'train': 12, 'dev_L0': 4, 'total_agent_limit': 16})
        baseline_path = controller.RUNTIME / 'dev-L0.json'
        controller.save(baseline_path,
                        controller.evaluation_config(controller.ids('dev'),
                                                     controller.read(controller.PUBLISHED / 'L0.json'), 4))
        with ThreadPoolExecutor(max_workers=2) as executor:
            baseline = executor.submit(controller.capture, 'dev-L0', baseline_path, env,
                                       probes['dev-preflight'])
            configs = sorted(controller.RUNTIME.glob('train-[0-9][0-9].json'))

            def start_training(number):
                config = configs[number]
                label = config.stem
                request = controller.read(config)
                workers = min(len(request['task_ids']), 16 if baseline.done() else 12)
                artifact = controller.read(controller.PUBLISHED / 'L0.json')
                controller.save(config, controller.evaluation_config(request['task_ids'], artifact, workers))
                future = executor.submit(controller.capture, label, config, env,
                    probes['train-preflight'] if label == 'train-01' else None)
                return future, {'batch': label, 'parallelism': workers}

            active, capture_stage = start_training(0)
            for number, config in enumerate(configs):
                label = config.stem
                controller.state(status='running', phase='capture_training', batch=label,
                    parallelism={'train': capture_stage['parallelism'],
                                 'dev_L0': 0 if baseline.done() else 4, 'total_agent_limit': 16})
                report, path = active.result()
                if number + 1 < len(configs):
                    active, capture_stage = start_training(number + 1)
                else:
                    capture_stage = None
                previous = path.parent / 'learning-status.json'
                if previous.is_file():
                    import appworld_learning
                    if appworld_learning.learning_usable(controller, controller.read(previous), 0):
                        continue
                controller.state(phase='learning', batch=label, metrics=controller.metrics(report['trials']),
                                 capture_in_parallel=capture_stage)
                result = controller.learn(report, path, env)
                if not result['usable']:
                    controller.state(status='learning_unresolved', batch=label, learning=result)
                    return
            artifact = controller.freeze_seed()
            controller.state(phase='waiting_cached_L0', version=controller.version(artifact))
            baseline_report, baseline_report_path = baseline.result()
        # Seed0 uses twelve workers for the twelve frozen tasks. The completed
        # L0 report is retained and is never re-run in this comparison stage.
        seed_config = controller.RUNTIME / 'dev-Seed0.json'
        controller.save(seed_config, controller.evaluation_config(controller.ids('dev'), artifact, 12))
        controller.state(phase='seed0_dev_comparison', version=controller.version(artifact),
                         parallelism={'Seed0': 12, 'dev_L0': 0, 'total_agent_limit': 16})
        seed_report, seed_report_path = controller.capture('dev-Seed0', seed_config, env)
        reports = {'L0': (baseline_report, baseline_report_path), 'Seed0': (seed_report, seed_report_path)}
        results = {name: {'report': str(path), 'metrics': controller.metrics(report['trials'])}
                   for name, (report, path) in reports.items()}
        by_variant = {name: {row['task_id']: row for row in report.get('trials', [])
            if row.get('execution_status') == 'completed'
            and controller.output(row).get('appworld_reward') in (0, 1)
            and not controller.output(row).get('error_code')}
            for name, (report, _) in reports.items()}
        paired = sorted(set(by_variant['L0']) & set(by_variant['Seed0']))
        controller.save(controller.GEPA / 'seed0-comparison.json', {
            'scope': plan['scope'], 'independent_holdout': False, 'results': results,
            'paired_tasks': len(paired), 'paired_metrics': {
                name: controller.metrics([by_variant[name][task] for task in paired])
                for name in ('L0', 'Seed0')}})
        controller.state(status='seed0_evaluated', phase='gap_review', results=results)
