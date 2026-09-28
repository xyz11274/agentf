"""真实 v1 评测入口的 daemon 冷启动故障回归。"""
from __future__ import annotations

import json
import os
from pathlib import Path
import sys
import time

import pytest


PACKAGE = Path(__file__).resolve().parents[1]
REPO = PACKAGE.parents[2]
sys.path[:0] = [str(PACKAGE/'evals'), str(PACKAGE/'harness'), str(REPO/'benchmark')]
import run_product_eval as entry


FAKE_DAEMON = r'''#!/usr/bin/env python3
import json
import os
from pathlib import Path
import socket
import sys
import time

config_path = Path(sys.argv[sys.argv.index('--config') + 1])
config = json.loads(config_path.read_text())
port = int(config['server']['listen'].rsplit(':', 1)[1])
records = Path(os.environ['FAKE_DAEMON_RECORDS'])
with records.open('a') as stream:
    stream.write(f'{os.getpid()} {port}\n')
attempt = len(records.read_text().splitlines())
mode = os.environ.get('FAKE_DAEMON_MODE', 'ready')
if mode == 'always_exit' or (mode == 'exit_once' and attempt == 1):
    raise SystemExit(23)
if mode == 'always_silent' or (mode == 'silent_once' and attempt == 1):
    time.sleep(60)
    raise SystemExit(24)

server = socket.socket()
server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
server.bind(('127.0.0.1', port))
server.listen()
Path(config['daemon']['pid_file']).write_text(str(os.getpid()))
while True:
    connection, _ = server.accept()
    with connection, connection.makefile('rwb') as stream:
        for line in stream:
            request = json.loads(line)
            if request['method'] == 'health':
                result = {'status': 'ok'}
            elif request['method'] == 'agent.list':
                result = {'agents': [{'agent_id': 'business-simulator'}]}
            else:
                result = {}
            stream.write((json.dumps({'jsonrpc': '2.0', 'id': request['id'],
                                      'result': result}) + '\n').encode())
            stream.flush()
'''


def _fake_daemon(tmp_path: Path) -> Path:
    executable = tmp_path/'fake-agentflowd'
    executable.write_text(FAKE_DAEMON)
    executable.chmod(0o755)
    return executable


def _start(tmp_path: Path, mode: str, attempts: int, attempt_timeout: float):
    executable = _fake_daemon(tmp_path)
    records = tmp_path/'starts.txt'
    env = {**os.environ, 'AGENTFLOW_DAEMON': str(executable),
           'FAKE_DAEMON_RECORDS': str(records), 'FAKE_DAEMON_MODE': mode}
    config = {'server': {}, 'daemon': {}}
    log = (tmp_path/'daemon.log').open('w')
    try:
        result = entry._start_business_daemon(
            config, tmp_path/'daemon.json', tmp_path, log, env,
            attempts=attempts, attempt_timeout=attempt_timeout)
    except BaseException:
        log.close()
        raise
    return result, records, log


def _assert_reaped(pid: int) -> None:
    with pytest.raises(ChildProcessError):
        os.waitpid(pid, os.WNOHANG)


def test_daemon_exit_is_reaped_and_retried_on_a_fresh_port(tmp_path):
    (process, port), records, log = _start(tmp_path, 'exit_once', 2, 1.0)
    try:
        starts = [tuple(map(int, line.split())) for line in records.read_text().splitlines()]
        assert len(starts) == 2
        assert starts[0][1] != starts[1][1] == port
        _assert_reaped(starts[0][0])
    finally:
        entry._stop_daemon(process)
        log.close()
    _assert_reaped(process.pid)


def test_unlistened_ports_fail_fast_with_bounded_retries_and_no_zombies(tmp_path):
    started = time.monotonic()
    with pytest.raises(RuntimeError, match='business_daemon_start_failed:port_not_listening'):
        _start(tmp_path, 'always_silent', 3, .15)
    assert time.monotonic() - started < 2
    starts = [tuple(map(int, line.split()))
              for line in (tmp_path/'starts.txt').read_text().splitlines()]
    assert len(starts) == 3
    assert len({port for _, port in starts}) == 3
    for pid, _ in starts:
        _assert_reaped(pid)
