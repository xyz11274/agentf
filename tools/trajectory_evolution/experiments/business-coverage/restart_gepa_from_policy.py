#!/usr/bin/env python3
"""Start a new scoring context from a frozen policy; preserve the previous tree."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import secrets
import select
import shutil
import subprocess
import urllib.error
import urllib.request

import offline_gepa as g


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--policy', required=True, type=Path)
    parser.add_argument('--baseline-report', required=True, type=Path)
    parser.add_argument('--seed-report', type=Path)
    parser.add_argument('--receipt', required=True, type=Path)
    args = parser.parse_args()
    with g.lifecycle_lock():
        existing_job_id = (g.coverage.read(args.receipt)['job_id']
                           if args.receipt.is_file() else '')
        plan, env = g.readiness()
        artifact = g.coverage.read(args.policy)
        imports = {'baseline': args.baseline_report, 'seed': args.seed_report}
        for kind, path in imports.items():
            if path is None:
                continue
            report = g.coverage.read(path)
            rows = report.get('trials', [])
            if (not report.get('complete') or len(rows) != 50 or
                    len({r['task_id'] for r in rows}) != 50 or
                    any(r.get('split') != 'val' or r.get('verdict') not in
                        ('passed', 'failed') for r in rows)):
                raise RuntimeError(kind + '_report_requires_50_scored_val_tasks')
        args.receipt.parent.mkdir(parents=True, exist_ok=True)
        backup = args.receipt.parent / 'runtime-before-restart'
        if not backup.exists():
            shutil.copytree(g.CONFIG, backup)
        config_path = g.CONFIG / 'gepa.json'
        config = g.coverage.read(config_path)
        for kind, path in imports.items():
            config.pop(kind + '_report_file', None)
            if path is not None:
                config[kind + '_report_file'] = os.path.relpath(path.resolve(), g.CONFIG)
        g.coverage.save(config_path, config)
        plan['config_files']['gepa.json'] = g.digest(config_path)
        g.coverage.save(g.PLAN, plan)
        command = [str(g.coverage.BINARY), 'serve', '--config', plan['service'],
                   '--no-auto-evaluate']
        child = None
        try:
            with (args.receipt.parent / 'bootstrap.log').open('a') as log:
                child = subprocess.Popen(command, cwd=g.ROOT, env=env,
                    stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=log,
                    text=True, start_new_session=True)
                ready, _, _ = select.select([child.stdout], [], [], 60)
                if not ready:
                    raise RuntimeError('gepa_management_start_timeout')
                line = child.stdout.readline().strip()
                prefix = 'trajectory-integration.v1 listening=127.0.0.1:'
                if not line.startswith(prefix) or not line[len(prefix):].isdigit():
                    raise RuntimeError('gepa_management_start_failed')
                port = int(line[len(prefix):])
                if not 0 < port < 65536:
                    raise RuntimeError('gepa_management_port_invalid')
                opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
                # A service shutdown leaves its old GEPA checkpoint resumable.
                # Explicitly cancel only the source policy's job before running
                # the new scoring context; retain its tree and reports.
                policy_path = args.policy.resolve()
                source_job = policy_path.parents[2].name
                expected_policy = (g.WORKSPACE / 'agent-evaluation' / source_job /
                                   'search' / 'versions' / policy_path.name).resolve()
                if (policy_path == expected_policy and source_job != existing_job_id and
                        g.progress(source_job).get('job_status') in ('pending', 'running')):
                    cancel = urllib.request.Request(
                        f'http://127.0.0.1:{port}/v1/invoke',
                        data=json.dumps({'operation': 'agent_evaluation.cancel',
                            'request_id': secrets.token_hex(16), 'job_id': source_job}).encode(),
                        headers={'Content-Type': 'application/json',
                            'Authorization': 'Bearer ' + env['EVOLUTION_BUSINESS_TOKEN']})
                    try:
                        with opener.open(cancel, timeout=180) as response:
                            cancelled = json.load(response)
                        if cancelled.get('ok') is not True:
                            raise RuntimeError('source_job_cancel_rejected')
                    except urllib.error.HTTPError as exc:
                        if (exc.code != 409 or g.progress(source_job).get('job_status')
                                in ('pending', 'running')):
                            raise
                    print(json.dumps({'source_job_cancel_requested': source_job}), flush=True)
                if existing_job_id:
                    job_id = existing_job_id
                    print(json.dumps({'resuming_job_id': job_id}), flush=True)
                else:
                    job_id = ''
                packet = {'operation': 'agent_evaluation.submit',
                          'request_id': secrets.token_hex(16), 'artifact': artifact}
                request = urllib.request.Request(
                    f'http://127.0.0.1:{port}/v1/invoke', data=json.dumps(packet).encode(),
                    headers={'Content-Type': 'application/json',
                             'Authorization': 'Bearer ' + env['EVOLUTION_BUSINESS_TOKEN']})
                if not job_id:
                    with opener.open(request, timeout=180) as response:
                        result = json.load(response)
                    if result.get('ok') is not True:
                        raise RuntimeError('gepa_submit_rejected')
                    job_id = result['result']['job_id']
                    g.coverage.save(args.receipt, {'job_id': job_id,
                        'source_policy': str(args.policy.resolve()),
                        'baseline_report': str(args.baseline_report.resolve())})
                    print(json.dumps({'submitted_job_id': job_id}), flush=True)
        finally:
            if child is not None:
                g._stop_process_group(child, 30)
                if child.stdout is not None:
                    child.stdout.close()
        print(json.dumps(g.run_to_publication(initial_job_id=job_id)), flush=True)


if __name__ == '__main__':
    main()
