"""Exercise seen-scene coverage, source provenance and exact split isolation."""
from collections import Counter, defaultdict
import json
from pathlib import Path
import random
import sys
import tempfile
import unittest
from zipfile import ZipFile

HERE = Path(__file__).resolve().parents[1] / "experiments/bfcl-v3"
sys.path.insert(0, str(HERE))
import bfcl_v3_dataset as dataset
import bfcl_v3_adapter as adapter


def fixture_rows():
    # Include tiny and large backend strata to exercise constrained rounding.
    backend_ids = [i for i, n in enumerate((1, 2, 3, 4, 10, 20, 40, 120)) for _ in range(n)]
    rows = []
    for scene, backend in enumerate(backend_ids):
        for variant in dataset.VARIANTS:
            rows.append({"id": f"{variant}_{scene}", "scenario": scene,
                         "subset": variant, "test_category": variant,
                         "involved_classes": [f"Backend{backend}"],
                         "split": "train" if scene < 100 else "val",
                         "turns": [[{"role": "user", "content": "Perform the task."}]]})
    return rows[:400], rows[400:]


class SeenScenarioTests(unittest.TestCase):
    def test_seen_scenes_disjoint_tasks_balanced_conditions_and_determinism(self):
        train, val = fixture_rows()
        split = dataset.split_seen_scenarios(train, val)
        self.assertEqual({s: len(v) for s, v in split.items()}, dataset.EXPECTED)
        all_ids = [r["id"] for rows in split.values() for r in rows]
        self.assertEqual(len(set(all_ids)), 800)
        scenes = {s: Counter(r["scenario"] for r in rows) for s, rows in split.items()}
        self.assertEqual(len(scenes["train"]), 200)
        self.assertTrue(all(n == 2 for counts in scenes.values() for n in counts.values()))
        self.assertEqual([len(scenes[s]) for s in ("search", "selection", "holdout")], [75, 25, 100])
        for left, right in (("search", "selection"), ("search", "holdout"), ("selection", "holdout")):
            self.assertFalse(set(scenes[left]) & set(scenes[right]))
        for s in scenes:
            grouped = defaultdict(set)
            for r in split[s]:
                grouped[r["scenario"]].add(r["subset"])
            self.assertTrue(all(len(v & set(dataset.VARIANTS[:2])) == 1 and
                                len(v & set(dataset.VARIANTS[2:])) == 1 for v in grouped.values()))
        expected = {"train": (100, 100, 100, 100), "search": (38, 37, 38, 37),
                    "selection": (12, 13, 12, 13), "holdout": (50, 50, 50, 50)}
        for s, counts in expected.items():
            self.assertEqual(Counter(r["subset"] for r in split[s]), dict(zip(dataset.VARIANTS, counts)))
        random.Random(17).shuffle(train)
        random.Random(29).shuffle(val)
        self.assertEqual(split, dataset.split_seen_scenarios(train, val))
        self.assertNotEqual(split, dataset.split_seen_scenarios(train, val, seed="other-fixed-seed"))

    def test_missing_variant_and_inconsistent_backend_are_rejected(self):
        train, val = fixture_rows()
        train[0] = dict(train[0], subset=dataset.VARIANTS[1])
        with self.assertRaises(ValueError):
            dataset.split_seen_scenarios(train, val)
        train, val = fixture_rows()
        train[0] = dict(train[0], involved_classes=["WrongBackend"])
        with self.assertRaises(ValueError):
            dataset.split_seen_scenarios(train, val)

    def test_freeze_preserves_old_source_and_adapter_tracks_original_members(self):
        train, val = fixture_rows()
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            original = root / "source.zip"
            with ZipFile(original, "w") as z:
                for member, rows in ((dataset.TRAIN_NAME, train), (dataset.VAL_NAME, val)):
                    z.writestr(member, "\n".join(json.dumps(r) for r in rows))
            old = root / "old"
            dataset.freeze(original, old)
            old_files = {p.name: p.read_bytes() for p in old.iterdir()}
            source = root / "seen/source"
            manifest = dataset.freeze_seen_scenarios(old / "bfcl_v3_splits.zip", source,
                                                      previous_source=old)
            self.assertTrue((source / "bfcl_v3_splits.zip").is_symlink())
            self.assertEqual({p.name: p.read_bytes() for p in old.iterdir()}, old_files)
            self.assertEqual(sum(manifest["old_memory_reuse"]["old_train_tasks_in_new_eval"].values()), 200)
            self.assertFalse(manifest["old_memory_reuse"]["allowed"])
            selected = adapter.selected_rows(source)
            native_counts = Counter()
            for logical_split, rows in selected.items():
                for row in rows:
                    task = adapter.evaluation_task(row, logical_split)
                    expected_member = dataset.TRAIN_NAME if row["scenario"] < 100 else dataset.VAL_NAME
                    self.assertEqual(task["initial_state"]["source_member"], expected_member)
                    native_counts[task["split"]] += 1
            self.assertEqual(native_counts, {"train": 550, "val": 50, "holdout": 200})
            with self.assertRaises(FileExistsError):
                dataset.freeze_seen_scenarios(original, source, previous_source=old)


if __name__ == "__main__":
    unittest.main()
