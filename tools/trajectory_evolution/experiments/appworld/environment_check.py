#!/usr/bin/env python3
"""Check original AppWorld execution and scoring with official TRAIN references.

No model is called. Results validate the environment and are not agent scores.
Run with runtime/venv/bin/python; hidden reference programs never enter prompts.
"""
import importlib.metadata
import json
import os
from pathlib import Path
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]
WORLD = HERE / "runtime/world"


def main() -> int:
    sys.dont_write_bytecode = True
    os.environ["APPWORLD_ROOT"] = str(WORLD)
    from appworld import AppWorld, update_root
    update_root(str(WORLD))
    results = []
    for task_id in ("34d9492_1", "29caf6f_1"):
        with AppWorld(task_id=task_id, experiment_name="environment-official-teacher-check",
                      load_ground_truth=True, ground_truth_mode="full") as world:
            body = world.task.ground_truth.compiled_solution_code_body
            world.execute(body() if callable(body) else body)
            tracker = world.evaluate()
            result = {"task_id": task_id, "official_reference_success": bool(tracker.success),
                      "test_count": tracker.num_tests, "pass_count": tracker.pass_count,
                      "fail_count": tracker.fail_count, "task_completed": world.task_completed()}
            results.append(result)
            if not tracker.success:
                raise RuntimeError("Official training reference failed original scoring: " + task_id)
    report = {"version": importlib.metadata.version("appworld"), "model_api_calls": 0,
              "verification_only": True, "not_agent_score": True, "results": results}
    output = ROOT / "trace_output/appworld/training/environment-check.json"
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, separators=(",", ":")) + "\n")
    print(json.dumps(report))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
