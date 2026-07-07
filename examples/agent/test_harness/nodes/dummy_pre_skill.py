#!/usr/bin/env python3
"""
Dummy Pre Skill — pre-processing hook placeholder.
"""

import json
import os
import sys


def log(msg: str):
    print(f"[flow] dummy_pre_skill: {msg}", file=sys.stderr)


def execute():
    user_input = os.environ.get("AF_INPUT", "")
    dialogue_id = os.environ.get("AF_DIALOGUE_ID", "")
    session_id = os.environ.get("AF_SESSION_ID", "")

    log(f"triggered session={session_id} dialogue={dialogue_id}")
    log(f"AF_INPUT={user_input[:80]}")
    log("passthrough — empty output, framework auto-forwards to coordinator")
    print(json.dumps({}, ensure_ascii=False))


if __name__ == "__main__":
    execute()
