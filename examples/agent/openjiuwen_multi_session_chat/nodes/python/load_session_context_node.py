#!/usr/bin/env python3
import sys
from pathlib import Path

NODE_ROOT = Path(__file__).resolve().parent
if str(NODE_ROOT) not in sys.path:
    sys.path.insert(0, str(NODE_ROOT))

from compat.runtime_bridge import DataBusBridge, current_session_id, emit_json


def main() -> None:
    session_id = current_session_id()
    databus = DataBusBridge()
    emit_json({"session_context": databus.get(f"session:{session_id}:context", "")})


if __name__ == "__main__":
    main()
