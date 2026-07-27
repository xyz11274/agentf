#!/usr/bin/env python3
"""
Dummy Pre Skill — pre-processing hook placeholder.

Also serves as the environment initialization gate: the workflow's first
node (shell_script) runs ``setup.sh`` before this python node executes.
"""

import json
import sys

from agentflow_runtime.common.runtime_context import (
    dialogue_id,
    input as af_input,
    session_id,
)


def log(msg: str):
    print(f"[flow] dummy_pre_skill: {msg}", file=sys.stderr)


def execute():
    user_input = af_input()
    dlg_id = dialogue_id()
    sess_id = session_id()

    log(f"triggered session={sess_id} dialogue={dlg_id}")
    log(f"AF_INPUT={user_input}")
    log("passthrough — empty output, framework auto-forwards to coordinator")
    print(json.dumps({}, ensure_ascii=False))


if __name__ == "__main__":
    execute()
