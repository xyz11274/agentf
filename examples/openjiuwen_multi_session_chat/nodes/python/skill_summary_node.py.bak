#!/usr/bin/env python3
import sys
from pathlib import Path

NODE_ROOT = Path(__file__).resolve().parent
if str(NODE_ROOT) not in sys.path:
    sys.path.insert(0, str(NODE_ROOT))

from compat.runtime_bridge import DataBusBridge, current_session_id, current_user_text, emit_json
from compat.skill_adapter import build_summary_result


def main() -> None:
    session_id = current_session_id()
    databus = DataBusBridge()
    payload = build_summary_result(
        current_user_text(),
        databus.get("partial_skill_results", ""),
        databus.get(f"session:{session_id}:history_json", "[]"),
    )
    databus.set(f"session:{session_id}:last_summary_result", payload.get("summary_result", ""))
    emit_json(payload)


if __name__ == "__main__":
    main()
