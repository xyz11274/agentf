#!/usr/bin/env python3
"""Run the actual business APIs/reminder worker with the AgentFlow daemon."""
from pathlib import Path
import os
import json
import signal
import subprocess
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'harness'))
from business_sim.backend import BusinessServer

# The daemon does not create parent directories for its Unix sockets.
if '--config' in sys.argv:
    config_path = Path(sys.argv[sys.argv.index('--config')+1])
    config = json.loads(config_path.read_text())
    for path in (config.get('server',{}).get('uds_biz_path'), config.get('server',{}).get('uds_ctrl_path'), config.get('daemon',{}).get('pid_file')):
        if path:
            Path(path).parent.mkdir(parents=True,exist_ok=True)

server = BusinessServer(('127.0.0.1', int(os.environ.get('BUSINESS_BACKEND_PORT','17894')))).start()
env = {**os.environ, 'BUSINESS_BACKEND_URL':server.url}
proc = None
try:
    print('Business APIs and reminder scheduler: '+server.url, flush=True)
    proc = subprocess.Popen(sys.argv[1:],env=env)
    def stop(_signum, _frame):
        if proc.poll() is None:
            proc.terminate()
    signal.signal(signal.SIGTERM,stop); signal.signal(signal.SIGINT,stop)
    code = proc.wait()
finally:
    if proc is not None and proc.poll() is None:
        proc.terminate()
        try:proc.wait(timeout=10)
        except subprocess.TimeoutExpired:proc.kill();proc.wait()
    server.close()
raise SystemExit(code)
