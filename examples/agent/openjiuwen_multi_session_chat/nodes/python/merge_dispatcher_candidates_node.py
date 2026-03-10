#!/usr/bin/env python3
import sys
from pathlib import Path

NODE_ROOT = Path(__file__).resolve().parent
if str(NODE_ROOT) not in sys.path:
    sys.path.insert(0, str(NODE_ROOT))

from compat.runtime_bridge import current_session_id, emit_json, env_text
from compat.skill_adapter import dump_jsonl, merge_dispatcher_lists


def main() -> None:
    session_id = current_session_id()
    merged = merge_dispatcher_lists(
        session_id,
        [
            env_text("AF_SEMANTIC_CANDIDATES"),
            env_text("AF_TEMPORAL_CANDIDATES"),
            env_text("AF_PLANNER_CANDIDATES"),
        ],
    )
    emit_json({"selected_skills": dump_jsonl(merged), "dispatcher_done": True})


if __name__ == "__main__":
    main()
