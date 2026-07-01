#!/usr/bin/env python3
"""
Dummy Pre Skill — pre-processing hook placeholder.

Sits between dispatcher and coordinator as the first next_skills target.
Pure passthrough: no processing, no DataBus writes, no next_skills output.
Framework auto-forwards skill completion to coordinator task, which recovers
the original user input from {dialogue_id}/user_prompt (agent.cpp:1796).

Reserved as a hook point for future pre-processing (validation, auth, etc.).
"""

import json
import os
import sys


def execute():
    # Pure passthrough — empty output.
    # Framework sees no next_skills → forward to coordinator task.
    # coordinator task recovers original input from user_prompt key.
    print(json.dumps({}, ensure_ascii=False))


if __name__ == "__main__":
    execute()
