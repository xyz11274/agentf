#!/usr/bin/env python3
"""最小真实 API 复现入口；不复制旧成绩、不写入凭据。"""
from __future__ import annotations
import argparse, os, subprocess, sys
from pathlib import Path
HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[3]
PYTHON=os.environ.get('AGENTFLOW_PYTHON', sys.executable)

def run(args):
    env=os.environ.copy()
    for k in ('HTTP_PROXY','HTTPS_PROXY','ALL_PROXY','http_proxy','https_proxy','all_proxy','BUSINESS_BROWSER_PROXY'):
        env.pop(k,None)
    env['NO_PROXY']=env['no_proxy']='*'; env['PYTHONDONTWRITEBYTECODE']='1'
    return subprocess.run([PYTHON,'-u',*args],cwd=ROOT,env=env,check=False).returncode

def main():
    p=argparse.ArgumentParser()
    g=p.add_mutually_exclusive_group(required=True)
    g.add_argument('--fresh',action='store_true',help='清理可重建产物并执行 train→GEPA→publish')
    g.add_argument('--resume',action='store_true',help='恢复已有 training/GEPA checkpoint')
    g.add_argument('--gepa',action='store_true',help='复用 training memory，重新执行 GEPA→publish')
    g.add_argument('--publish',action='store_true',help='仅发布已通过门禁的现有候选')
    g.add_argument('--status',action='store_true')
    ns=p.parse_args()
    if ns.fresh: cmd=['tools/trajectory_evolution/experiments/business-coverage/offline_experiment.py','--fresh']
    elif ns.resume: cmd=['tools/trajectory_evolution/experiments/business-coverage/offline_experiment.py','--resume']
    elif ns.gepa:
        # Keep the public entry point on the shared lifecycle.  GEPA is not a
        # second pipeline that can bypass the common gates or publication
        # receipt; --resume selects the next durable stage automatically.
        cmd=['tools/trajectory_evolution/experiments/business-coverage/offline_experiment.py','--resume']
    elif ns.publish:
        # Publication is the final common stage.  Re-entering the lifecycle is
        # idempotent and prevents direct native publishing of stale artifacts.
        cmd=['tools/trajectory_evolution/experiments/business-coverage/offline_experiment.py','--resume']
    else: cmd=['tools/trajectory_evolution/experiments/business-coverage/offline_experiment.py','--status']
    return run(cmd)
if __name__=='__main__': raise SystemExit(main())
