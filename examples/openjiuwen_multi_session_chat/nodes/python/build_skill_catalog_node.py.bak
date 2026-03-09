#!/usr/bin/env python3
import json
import sys
from pathlib import Path

NODE_ROOT = Path(__file__).resolve().parent
if str(NODE_ROOT) not in sys.path:
    sys.path.insert(0, str(NODE_ROOT))

from compat.runtime_bridge import emit_json
from compat.skill_adapter import skill_catalog_json


def main() -> None:
    emit_json({"skill_catalog": skill_catalog_json()})


if __name__ == "__main__":
    main()
