#!/usr/bin/env python3
"""Coordinator node — drives the harness loop's termination decision.

Triggered by the framework after every skill completes. It loads accumulated
results + loop state from the shared Store, calls the pure ``decide_next`` core,
persists updated state, and either emits ``next_skills`` to continue, or an empty
list plus ``response`` to terminate (termination is the coordinator's job).
"""
import json
import sys
from pathlib import Path

NODE_DIR = Path(__file__).resolve().parent
if str(NODE_DIR) not in sys.path:
    sys.path.insert(0, str(NODE_DIR))

import harness_bus as hb  # noqa: E402
import harness_lib as hl  # noqa: E402


def main():
    catalog = hl.load_skills_catalog()
    config = hl.load_harness_config()

    store = hb.Store()
    env_text = hb.user_text()
    if env_text and env_text not in ("workflow_completed", "workflow_scheduled"):
        store.save_user_text(env_text)
    results = store.load_results()
    state = store.load_state() or hl.new_state()

    available_parameters = {"user_text": store.load_user_text() or env_text}
    out = hl.decide_next(state, catalog, results, config, available_parameters)
    store.save_state(out["state"])

    if out["next_skills"]:
        nxt = out["next_skills"][0]
        payload = json.dumps([{"name": s} for s in out["next_skills"]],
                             ensure_ascii=False)
        hb.write_marker("decision", "NEXT:" + nxt)
        hb.emit_json({
            "next_skills": payload,
            "selected_skills": payload,
            "coordinator_done": False,
            "iteration": out["state"].get("iteration", 0),
        })
    else:
        hb.write_marker("response", out["response"])
        hb.write_marker("decision", "DONE")
        hb.emit_json({
            "response": out["response"],
            "final_response": out["response"],
            "coordinator_done": True,
            "terminated_reason": out.get("terminated_reason", ""),
            "iteration": out["state"].get("iteration", 0),
        })


if __name__ == "__main__":
    main()
