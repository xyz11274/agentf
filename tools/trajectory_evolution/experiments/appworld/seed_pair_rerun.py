"""Run a same-revision L0/Seed pair on already completed task IDs.

This is a deliberately small comparison rerun. It never edits the old L0
report; both variants use the current frozen host revision and the same tasks.
"""
from __future__ import annotations

import json
from pathlib import Path

import appworld_experiment as c

ROOT = c.ROOT
OLD = ROOT / "trace_output/appworld/gepa/captures/dev-L0/report.json"
OUT = ROOT / "trace_output/appworld/gepa/seed-pair-v3"
RUNTIME = c.RUNTIME / "seed-pair-v3"


def main() -> None:
    old = c.read(OLD)
    rows = [row for row in old.get("trials", [])
            if row.get("execution_status") == "completed"
            and not (row.get("evidence", {}).get("output", {}).get("error_code"))]
    # Fixed, reproducible selection from the completed L0 cohort; no score sort.
    rows = sorted(rows, key=lambda row: row["task_id"])[:12]
    task_ids = [row["task_id"] for row in rows]
    if len(task_ids) < 1:
        raise RuntimeError("no_completed_l0_tasks_for_seed_pair")
    baseline = {"schema_version": 1,
                "agent": {"name": c.AGENT, "revision": c.host_version()},
                "user_id": c.OWNER, "level": "L0", "policy": None}
    seed = c.read(c.PUBLISHED / "seed0/strategy.json")
    if seed["agent"]["revision"] != baseline["agent"]["revision"]:
        raise RuntimeError("seed_host_revision_mismatch")
    RUNTIME.mkdir(parents=True, exist_ok=True)
    OUT.mkdir(parents=True, exist_ok=True)
    env = c.environment()
    l0_cfg = RUNTIME / "l0.json"
    seed_cfg = RUNTIME / "seed0.json"
    c.save(l0_cfg, c.evaluation_config(task_ids, baseline, min(8, len(task_ids))))
    c.save(seed_cfg, c.evaluation_config(task_ids, seed, min(8, len(task_ids))))
    l0, l0_path = c.capture("seed-pair-v3-L0", l0_cfg, env)
    seed_report, seed_path = c.capture("seed-pair-v3-Seed0", seed_cfg, env)

    def valid(report):
        return {row["task_id"]: row for row in report.get("trials", [])
                if row.get("execution_status") == "completed"
                and not c.output(row).get("error_code")
                and c.output(row).get("appworld_reward") in (0, 1)}
    valid_l0, valid_seed = valid(l0), valid(seed_report)
    paired = sorted(set(valid_l0) & set(valid_seed))
    result = {"scope": "completed_l0_fixed_hash_subset",
              "task_ids": task_ids, "paired_tasks": len(paired),
              "host_version": c.host_version(), "seed_version": c.version(seed),
              "old_l0_report_reused_for_selection_only": str(OLD),
              "old_l0_report_modified": False,
              "reports": {"L0": str(l0_path), "Seed0": str(seed_path)},
              "metrics": {"L0": c.metrics(l0.get("trials", [])),
                          "Seed0": c.metrics(seed_report.get("trials", []))},
              "paired_metrics": {"L0": c.metrics([valid_l0[k] for k in paired]),
                                  "Seed0": c.metrics([valid_seed[k] for k in paired])}}
    c.save(OUT / "comparison.json", result)
    print(json.dumps(result, ensure_ascii=False))


if __name__ == "__main__":
    main()
