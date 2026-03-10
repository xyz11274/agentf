#!/usr/bin/env python3
import sys
from pathlib import Path

NODE_ROOT = Path(__file__).resolve().parent
if str(NODE_ROOT) not in sys.path:
    sys.path.insert(0, str(NODE_ROOT))

from compat.runtime_bridge import current_session_id, emit_json, env_text
from compat.skill_adapter import merge_coordinator_lists


def main() -> None:
    session_id = current_session_id()
    payload = merge_coordinator_lists(
        session_id,
        env_text("AF_SELECTED_SKILLS"),
        env_text("AF_PARTIAL_SKILL_RESULTS"),
        [
            env_text("AF_RESULT_AWARE_CANDIDATES"),
            env_text("AF_DEPENDENCY_CANDIDATES"),
            env_text("AF_FINALIZER_CANDIDATES"),
        ],
    )
    emit_json(payload)


if __name__ == "__main__":
    main()
