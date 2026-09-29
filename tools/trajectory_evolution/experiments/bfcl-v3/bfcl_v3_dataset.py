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

``--seen-scenarios`` instead reserves two complementary variants per scenario
for training, with the other two assigned to one evaluation pool. It writes a
new source directory and shares the immutable archive with the original split.
"""

from __future__ import annotations

import argparse
from collections import Counter, defaultdict
import hashlib
import json
import os
from pathlib import Path
import shutil
from zipfile import BadZipFile, ZipFile


TRAIN_NAME = "splits/multi_turn_train.jsonl"
VAL_NAME = "splits/multi_turn_val.jsonl"
EXPECTED = {"train": 400, "search": 150, "selection": 50, "holdout": 200}
VARIANTS = ("multi_turn_base", "multi_turn_long_context", "multi_turn_miss_func",
            "multi_turn_miss_param")
SEEN_SEED = "bfcl-v3-seen-scenarios-v1"
EVAL_PAIRS = ((VARIANTS[0], VARIANTS[2]), (VARIANTS[0], VARIANTS[3]),
              (VARIANTS[1], VARIANTS[2]), (VARIANTS[1], VARIANTS[3]))


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
        raise ValueError("BFCL scenarios must contain four variants")
    for items in groups.values():
        if {str(item.get("subset")) for item in items} != {
            "multi_turn_base", "multi_turn_long_context", "multi_turn_miss_func",
            "multi_turn_miss_param",
        }:
            raise ValueError("BFCL scenario variants are incomplete")
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


def _rank(seed: str, *parts: str) -> str:
    return sha256_bytes((seed + ":" + ":".join(parts)).encode("utf-8"))


def _stratified_assign(groups: dict[str, list[str]], quotas: dict[str, int],
                       seed: str) -> dict[str, list[str]]:
    """Apportion exact quotas over metadata strata with deterministic rounding.

    Start with proportional floors, then allocate each remaining slot to the
    available cell with smallest increase in squared proportional error. This
    uses only stratum sizes, scenario IDs and the fixed seed, never outcomes.
    """
    total = sum(len(items) for items in groups.values())
    if total != sum(quotas.values()) or total <= 0:
        raise ValueError("bfcl_stratum_quota_mismatch")
    cells = {(group, label): len(items) * quota // total
             for group, items in groups.items() for label, quota in quotas.items()}
    remaining_rows = {g: len(v) - sum(cells[g, k] for k in quotas) for g, v in groups.items()}
    remaining_cols = {k: n - sum(cells[g, k] for g in groups) for k, n in quotas.items()}
    while any(remaining_rows.values()):
        g, k = min(((g, k) for g in groups for k in quotas
                    if remaining_rows[g] and remaining_cols[k]),
                   key=lambda cell: ((2 * cells[cell] + 1) * total
                                     - 2 * len(groups[cell[0]]) * quotas[cell[1]],
                                     _rank(seed, *cell)))
        cells[g, k] += 1
        remaining_rows[g] -= 1
        remaining_cols[k] -= 1
    result = {k: [] for k in quotas}
    for g in sorted(groups):
        items = sorted(groups[g], key=lambda item: _rank(seed, g, item))
        offset = 0
        for k in sorted(quotas, key=lambda label: _rank(seed, g, label)):
            count = cells[g, k]
            result[k].extend(items[offset:offset + count])
            offset += count
    return result


def split_seen_scenarios(train: list[dict], val: list[dict], *, seed: str = SEEN_SEED
                         ) -> dict[str, list[dict]]:
    """Two train variants per scene; two complementary variants in one eval pool."""
    if len(train) != 400 or len(val) != 400:
        raise ValueError("bfcl_seen_source_counts_invalid")
    source = train + val
    if len({r["id"] for r in source}) != 800:
        raise ValueError("bfcl_seen_source_ids_overlap")
    scenes = _scenario_groups(source)
    if len(scenes) != 200:
        raise ValueError("bfcl_seen_scenario_count_invalid")
    strata: dict[str, list[str]] = defaultdict(list)
    scene_strata = {}
    for scene, items in scenes.items():
        backends = {tuple(sorted(row.get("involved_classes", []))) for row in items}
        if len(backends) != 1 or not next(iter(backends)):
            raise ValueError("bfcl_seen_scenario_backends_inconsistent")
        stratum = "+".join(next(iter(backends)))
        strata[stratum].append(scene)
        scene_strata[scene] = stratum
    pools = _stratified_assign(strata, {"search": 75, "selection": 25, "holdout": 100},
                               seed + ":pools")
    pair_quotas = {"search": (19, 19, 19, 18), "selection": (6, 6, 6, 7),
                   "holdout": (25, 25, 25, 25)}
    result = {k: [] for k in EXPECTED}
    for pool, scene_ids in pools.items():
        pool_strata: dict[str, list[str]] = defaultdict(list)
        for scene in scene_ids:
            pool_strata[scene_strata[scene]].append(scene)
        pairs = _stratified_assign(pool_strata,
                                  {str(i): n for i, n in enumerate(pair_quotas[pool])},
                                  seed + ":pairs:" + pool)
        for pair, assigned in pairs.items():
            for scene in assigned:
                for row in scenes[scene]:
                    destination = pool if row["subset"] in EVAL_PAIRS[int(pair)] else "train"
                    result[destination].append(row)
    for split in result:
        result[split].sort(key=lambda row: _rank(seed, "row-order", row["id"]))
    if {k: len(v) for k, v in result.items()} != EXPECTED:
        raise AssertionError("bfcl_seen_split_counts_invalid")
    return result


def freeze_seen_scenarios(source_zip: Path, output: Path, *, previous_source: Path,
                          seed: str = SEEN_SEED) -> dict:
    """Create a small independent source directory; preserve old IDs, share the zip."""
    source_zip, output = source_zip.resolve(), output.resolve()
    previous_source = previous_source.resolve()
    if output.exists():
        raise FileExistsError(f"refusing to overwrite frozen BFCL source: {output}")
    previous_manifest = json.loads((previous_source / "manifest.json").read_text())
    previous_ids = {s: set((previous_source / f"{s}_ids.txt").read_text().splitlines())
                    for s in EXPECTED}
    with ZipFile(source_zip) as archive:
        train, val = _rows(archive, TRAIN_NAME), _rows(archive, VAL_NAME)
    splits = split_seen_scenarios(train, val, seed=seed)
    source_digest = sha256_bytes(source_zip.read_bytes())
    if source_digest != previous_manifest["source_zip_sha256"]:
        raise ValueError("bfcl_seen_original_archive_mismatch")
    manifest = {
        "schema_version": 1, "dataset": "bfcl-v3-multi-turn-seen-scenarios-v1",
        "source_url": previous_manifest.get("source_url"),
        "source_zip_sha256": source_digest,
        "source_members": previous_manifest["source_members"],
        "split_protocol": "seen-scenarios-v1: each of 200 scenarios has train2+eval2; eval scenarios search75/selection25/holdout100",
        "split_seed": seed,
        "counts": {s: len(v) for s, v in splits.items()},
        "scenario_counts": {s: len({r['scenario'] for r in v}) for s, v in splits.items()},
        "variant_counts": {s: dict(sorted(Counter(r["subset"] for r in v).items()))
                           for s, v in splits.items()},
        "tool_combo_scenario_counts": {
            s: dict(sorted(Counter("+".join(sorted(v[0]["involved_classes"]))
                                    for v in _group_rows_by_scenario(rows).values()).items()))
            for s, rows in splits.items()},
        "ids": {s: f"{s}_ids.txt" for s in EXPECTED},
        "search_selection_scenario_overlap": [],
        "protocol_semantics": "Seen scenario, held-out condition variant. Exact task IDs are disjoint. Evaluation pools have disjoint scenarios; all are exposed through two training variants.",
        "stratification": "Exact available backend-set strata; proportional floors plus deterministic squared-error rounding; complementary normal/context + missing-function/parameter pairs; metadata only, no scores or target calls.",
        "old_memory_reuse": {"allowed": False, "reason": "Old training contains exact tasks in the new evaluation pools; rebuild memory from the new train IDs.",
                            "old_train_tasks_in_new_eval": {
                                s: len(previous_ids["train"] & {r["id"] for r in splits[s]})
                                for s in ("search", "selection", "holdout")},
                            "old_train_tasks_retained_in_new_train": len(previous_ids["train"] & {r["id"] for r in splits["train"]})},
        "security": previous_manifest.get("security", {}),
    }
    output.mkdir(parents=True)
    # The immutable 800-row archive is shared; no duplicate schemas or fixture state.
    (output / "bfcl_v3_splits.zip").symlink_to(os.path.relpath(source_zip, output))
    provenance = output / "provenance"
    provenance.mkdir()
    shutil.copy2(previous_source / "manifest.json", provenance / "previous-manifest.json")
    for s, rows in splits.items():
        (output / f"{s}_ids.txt").write_text("".join(r["id"] + "\n" for r in rows))
        shutil.copy2(previous_source / f"{s}_ids.txt", provenance / f"previous-{s}_ids.txt")
    (output / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n")
    return manifest


def _group_rows_by_scenario(rows: list[dict]) -> dict[str, list[dict]]:
    result: dict[str, list[dict]] = defaultdict(list)
    for row in rows:
        result[str(row["scenario"])].append(row)
    return dict(result)


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
    parser.add_argument("--seen-scenarios", action="store_true")
    parser.add_argument("--previous-source", type=Path)
    parser.add_argument("--seed", default=SEEN_SEED)
    args = parser.parse_args(argv)
    if args.seen_scenarios:
        if args.previous_source is None:
            parser.error("--seen-scenarios requires --previous-source")
        manifest = freeze_seen_scenarios(args.source_zip, args.output,
                                         previous_source=args.previous_source, seed=args.seed)
    else:
        manifest = freeze(args.source_zip, args.output)
    print(json.dumps(manifest, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
