#!/usr/bin/env python3
"""No-model inspection, state reset and direct tool calls for this simulator."""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "harness"))
from business_sim.contracts import SERVICES
from business_sim.services import dispatch
from business_sim.state import Store, scope_key


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=["call", "state", "events", "reset", "services"])
    parser.add_argument("--user", default="demo-user")
    parser.add_argument("--client", default="demo-chat")
    parser.add_argument("--skill", choices=sorted(SERVICES))
    parser.add_argument("--args", default="{}", help="JSON tool arguments")
    args = parser.parse_args()
    store, scope = Store(), scope_key(args.user, args.client)
    if args.command == "call":
        if not args.skill:
            parser.error("call requires --skill")
        result = dispatch(args.skill, json.loads(args.args), store=store, scope=scope)
    elif args.command == "reset":
        store.reset(scope)
        result = {"reset": True, "user": args.user, "client": args.client}
    elif args.command == "services":
        result = {s: {"legacy_agent": m[0], "description": m[1], "actions": m[2]} for s, m in SERVICES.items()}
    else:
        result = getattr(store, "read" if args.command == "state" else "events")(scope)
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return int(isinstance(result, dict) and result.get("status", "succeeded") != "succeeded")


if __name__ == "__main__":
    raise SystemExit(main())
