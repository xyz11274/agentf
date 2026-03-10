#!/usr/bin/env python3
import sys
from pathlib import Path

NODE_ROOT = Path(__file__).resolve().parent
if str(NODE_ROOT) not in sys.path:
    sys.path.insert(0, str(NODE_ROOT))

from compat.runtime_bridge import emit_json, current_user_text
from compat.skill_adapter import build_date_result


def main() -> None:
    emit_json(build_date_result(current_user_text()))


if __name__ == "__main__":
    main()
