#!/usr/bin/env python3
"""Freeze the BFCL v3 multi-turn source and its independent evaluation splits.

The archive contains 400 training cases and 400 validation cases.  The
validation cases are grouped by ``scenario`` and each scenario has four
variants (base, long-context, missing-function and missing-parameter).  We
keep whole scenarios for the final 200-case holdout.  The remaining 200
cases are divided into 150 search and 50 selection cases; one scenario is
split 2/2 because four-case groups cannot make both numbers exactly.

Only the archive and small ID manifests are retained.  A later runner can
materialize one native JSONL view in a temporary directory, so the large
function schemas and synthetic fixture state are not duplicated in the
experiment output.
"""

from __future__ import annotations

import argparse
from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path
import shutil
from zipfile import BadZipFile, ZipFile


TRAIN_NAME = "splits/multi_turn_train.jsonl"
VAL_NAME = "splits/multi_turn_val.jsonl"
EXPECTED = {"train": 400, "search": 150, "selection": 50, "holdout": 200}


def sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def _rows(archive: ZipFile, name: str) -> list[dict]:
    rows = [json.loads(line) for line in archive.read(name).splitlines() if line.strip()]
    if not all(isinstance(row, dict) and row.get("id") for row in rows):
        raise ValueError(f"invalid BFCL rows: {name}")
    if len({row["id"] for row in rows}) != len(rows):
        raise ValueError(f"duplicate BFCL ids: {name}")
    return rows


def _scenario_groups(rows: list[dict]) -> dict[str, list[dict]]:
    groups: dict[str, list[dict]] = defaultdict(list)
    for row in rows:
        groups[str(row.get("scenario"))].append(row)
    if any(len(items) != 4 for items in groups.values()):
        raise ValueError("BFCL validation scenarios must contain four variants")
    for items in groups.values():
        if {str(item.get("subset")) for item in items} != {
            "multi_turn_base", "multi_turn_long_context", "multi_turn_miss_func",
            "multi_turn_miss_param",
        }:
            raise ValueError("BFCL validation scenario variants are incomplete")
    return dict(groups)


def split_rows(train: list[dict], val: list[dict]) -> dict[str, list[dict]]:
    """Return deterministic train/search/selection/holdout rows.

    The order is a hash order over scenario IDs, independent of filesystem or
    archive member order.  The selected two rows are the first two IDs in one
    otherwise selection-only scenario; this is recorded in the manifest as a
    small, explicit search/selection overlap.
    """
    if len(train) != 400 or len(val) != 400:
        raise ValueError(f"BFCL source counts changed: train={len(train)} val={len(val)}")
    groups = _scenario_groups(val)
    ordered = sorted(groups, key=lambda scenario: hashlib.sha256(
        ("bfcl-v3-multi-turn:" + scenario).encode("utf-8")).hexdigest())
    holdout_scenarios = ordered[:50]
    remaining = ordered[50:]
    selection_scenarios = remaining[:13]
    search_scenarios = remaining[13:]
    # 37 complete scenarios = 148.  Move two variants from the first
    # selection scenario to search to reach the requested 150/50 exactly.
    partial = selection_scenarios[0]
    partial_rows = sorted(groups[partial], key=lambda row: row["id"])
    moved = partial_rows[:2]
    selection_rows = [row for scenario in selection_scenarios for row in groups[scenario]
                      if row not in moved]
    search_rows = [row for scenario in search_scenarios for row in groups[scenario]] + moved
    holdout_rows = [row for scenario in holdout_scenarios for row in groups[scenario]]
    result = {"train": list(train), "search": search_rows,
              "selection": selection_rows, "holdout": holdout_rows}
    if {key: len(value) for key, value in result.items()} != EXPECTED:
        raise AssertionError("BFCL split count construction failed")
    all_ids = [row["id"] for rows in result.values() for row in rows]
    if len(set(all_ids)) != len(all_ids):
        raise AssertionError("BFCL split IDs overlap")
    return result


def freeze(source_zip: Path, output: Path) -> dict:
    """Copy the immutable source and write a small reproducibility manifest."""
    source_zip = source_zip.resolve()
    output = output.resolve()
    if output.exists():
        raise FileExistsError(f"refusing to overwrite frozen BFCL source: {output}")
    try:
        archive_bytes = source_zip.read_bytes()
        with ZipFile(source_zip) as archive:
            if archive.testzip() is not None:
                raise ValueError("BFCL zip has a corrupt member")
            train_bytes = archive.read(TRAIN_NAME)
            val_bytes = archive.read(VAL_NAME)
            train = _rows(archive, TRAIN_NAME)
            val = _rows(archive, VAL_NAME)
    except BadZipFile as exc:
        raise ValueError("invalid BFCL zip") from exc
    splits = split_rows(train, val)
    output.mkdir(parents=True)
    shutil.copy2(source_zip, output / "bfcl_v3_splits.zip")
    for name, rows in splits.items():
        (output / f"{name}_ids.txt").write_text(
            "".join(row["id"] + "\n" for row in rows), encoding="utf-8")
    scenario_sets = {}
    for name in ("search", "selection", "holdout"):
        scenario_sets[name] = sorted({str(row.get("scenario")) for row in splits[name]})
    partial_overlap = sorted(set(scenario_sets["search"]) & set(scenario_sets["selection"]))
    manifest = {
        "schema_version": 1,
        "dataset": "bfcl-v3-multi-turn",
        "source_url": "http://106.55.172.74:8080/tmp/bfcl_v3_splits.zip",
        "source_zip_sha256": sha256_bytes(archive_bytes),
        "source_members": {
            "train": {"member": TRAIN_NAME, "sha256": sha256_bytes(train_bytes), "count": len(train)},
            "val": {"member": VAL_NAME, "sha256": sha256_bytes(val_bytes), "count": len(val)},
        },
        "split_protocol": "train400; val400 -> holdout50_scenarios(200), search37_scenarios+2_rows(150), selection12_scenarios+2_rows(50)",
        "counts": {name: len(rows) for name, rows in splits.items()},
        "scenario_counts": {name: len(scenarios) for name, scenarios in scenario_sets.items()},
        "search_selection_scenario_overlap": partial_overlap,
        "variant_counts": {
            name: dict(sorted(Counter(str(row.get("subset")) for row in rows).items()))
            for name, rows in splits.items()
        },
        "ids": {name: f"{name}_ids.txt" for name in splits},
        "security": {
            "raw_fixture_contains_synthetic_password_fields": True,
            "native_adapter_must_not_copy_initial_config_or_credentials_to_task_attributes": True,
        },
    }
    (output / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
                                             encoding="utf-8")
    return manifest


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source_zip", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args(argv)
    print(json.dumps(freeze(args.source_zip, args.output), ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
