import json
import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

MODULE = Path(__file__).with_name("best_strategy.py")
sys.path.insert(0, str(MODULE.parent))
SPEC = importlib.util.spec_from_file_location("best_strategy_under_test", MODULE)
best_strategy = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(best_strategy)


def _trial(task_id, variant, quality, version):
    return {
        "task_id": task_id,
        "split": "holdout",
        "variant_id": variant,
        "verdict": "passed" if quality >= .5 else "failed",
        "duration_ms": 100,
        "metrics": {"tokens": 1000, "llm_calls": 2},
        "evidence": {
            "loaded_version": version,
            "assessment": {"quality": quality},
            "loop_review": {"wasted_iterations": []},
        },
    }


class BestStrategyTest(unittest.TestCase):
    def _make_job(self, base: Path, final_nodes=(1, 2, 3)):
        root = base / "job"
        (root / "search/versions").mkdir(parents=True)
        (root / "baseline.json").write_text(json.dumps({"node": "l0"}))
        (root / "plan.json").write_text(json.dumps({"dataset_version": "dataset-test"}))
        evaluations = {}
        nodes = []
        for node_id in range(5):
            version = f"v{node_id}"
            (root / "search/versions" / f"{node_id}.json").write_text(
                json.dumps({"node": node_id}))
            nodes.append({
                "id": node_id,
                "version": version,
                "validation": {f"task-{i}": {
                    "quality": .5 + node_id / 100,
                    "strict_pass_rate": .5,
                    "invalid_loops": 1,
                    "duration_ms": 1000,
                    "tokens": 1000,
                } for i in range(200)},
                "optimization": {"feasible": node_id == 1, "passed": node_id == 1,
                                  "score": 100 - node_id},
            })
        candidates = []
        holdout_quality = {1: .7, 2: .9, 3: .8}
        for rank, node_id in enumerate(final_nodes, 1):
            version = f"v{node_id}"
            rel = f"evaluations/e{node_id}/attempt-1/report.json"
            report_path = root / "search" / rel
            report_path.parent.mkdir(parents=True, exist_ok=True)
            trials = []
            quality = holdout_quality.get(node_id, .6)
            for task_index in range(200):
                task_id = f"holdout-{task_index}"
                trials.append(_trial(task_id, "baseline", .5, "l0-version"))
                trials.append(_trial(task_id, "candidate", quality, version))
            report = {"complete": True, "dataset_version": "dataset-test", "trials": trials}
            raw = json.dumps(report).encode()
            report_path.write_bytes(raw)
            evaluations[f"eval-{node_id}"] = {
                "split": "holdout", "status": "complete", "version": version,
                "baseline_version": "l0-version", "dataset_version": "dataset-test",
                "task_ids": [f"holdout-{i}" for i in range(200)],
                "report": rel, "report_hash": best_strategy._sha256_bytes(raw),
            }
            candidates.append({"rank": rank, "node": node_id, "version": version,
                               "status": "complete"})
        tree = {
            "status": "completed", "winner": 1, "nodes": nodes,
            "final_holdout": {"status": "completed", "candidates": candidates},
            "evaluations": evaluations,
        }
        tree_path = root / "search/tree.json"
        tree_path.parent.mkdir(parents=True, exist_ok=True)
        tree_path.write_text(json.dumps(tree))
        return root, tree_path

    def test_choose_best_is_posthoc_within_frozen_finalists_by_holdout(self):
        with tempfile.TemporaryDirectory() as directory:
            root, _ = self._make_job(Path(directory))
            fake_splits = {f"holdout-{i}": "holdout" for i in range(200)}
            with patch.object(best_strategy, "JOB_ROOT", root.parent), \
                    patch.object(best_strategy, "_source_rows",
                                 return_value=([], fake_splits)), \
                    patch.object(best_strategy, "_artifact_version", return_value="l0-version"):
                chosen = best_strategy.choose_best("job")
            # Node 4 has the strongest validation but is outside the frozen
            # three. Node 1 has a feasible GEPA gate; holdout quality wins.
            self.assertEqual(chosen["node"], 2)
            self.assertEqual(chosen["selection_reason"],
                             "posthoc_best_of_frozen_and_validation_fallback_candidates")
            self.assertTrue(chosen["showcase_only"])
            self.assertFalse(chosen["formal_gate_passed"])
            self.assertEqual(chosen["holdout"]["completed"], 200)
            self.assertEqual(chosen["l0_holdout"]["completed"], 200)
            self.assertEqual(chosen["showcase_candidate_count"], 3)
            self.assertEqual({row["node"] for row in chosen["showcase_candidates"]}, {1, 2, 3})

    def test_short_candidate_pool_returns_explicit_best_available(self):
        with tempfile.TemporaryDirectory() as directory:
            root, tree_path = self._make_job(Path(directory), final_nodes=(1,))
            tree = json.loads(tree_path.read_text())
            for node in tree["nodes"][2:]:
                node["validation"] = {}
            tree["evaluations"] = {key: value for key, value in tree["evaluations"].items()
                                   if value["version"] == "v1"}
            tree_path.write_text(json.dumps(tree))
            fake_splits = {f"holdout-{i}": "holdout" for i in range(200)}
            with patch.object(best_strategy, "JOB_ROOT", root.parent), \
                    patch.object(best_strategy, "_source_rows",
                                 return_value=([], fake_splits)), \
                    patch.object(best_strategy, "_artifact_version", return_value="l0-version"):
                chosen = best_strategy.choose_best("job")
            self.assertEqual(chosen["node"], 1)
            self.assertEqual(chosen["selection_reason"],
                             "posthoc_best_of_frozen_and_validation_fallback_candidates")
            self.assertEqual(chosen["candidate_scope"]["shortfall"], 2)
            self.assertEqual(chosen["candidate_scope"]["frozen"], 1)

    def test_holdout_reuse_rejects_mismatched_dataset_version(self):
        with tempfile.TemporaryDirectory() as directory:
            root, tree_path = self._make_job(Path(directory), final_nodes=(1,))
            tree = json.loads(tree_path.read_text())
            tree["evaluations"]["eval-1"]["dataset_version"] = "stale-dataset"
            tree_path.write_text(json.dumps(tree))
            candidate = best_strategy._candidate_pool(root, tree)[0]
            with patch.object(best_strategy, "_source_rows",
                               return_value=([], {f"holdout-{i}": "holdout" for i in range(200)})), \
                    patch.object(best_strategy, "_artifact_version", return_value="l0-version"):
                self.assertIsNone(best_strategy._finalist_report(root, tree, candidate))

    def test_pool_backfill_freezes_val_rank_before_supplemental_holdout(self):
        with tempfile.TemporaryDirectory() as directory:
            root, tree_path = self._make_job(Path(directory), final_nodes=(1,))
            tree = json.loads(tree_path.read_text())
            for node in tree["nodes"][2:]:
                node["validation"] = {}
            for node_id in (2, 3, 4):
                relative = f"evaluations/pool-{node_id}/attempt-1/report.json"
                report_path = root / "search" / relative
                report_path.parent.mkdir(parents=True, exist_ok=True)
                report_path.write_text(json.dumps({"complete": True,
                                                   "dataset_version": "dataset-test"}))
                tree["evaluations"][f"pool-{node_id}"] = {
                    "split": "train", "status": "complete", "version": f"v{node_id}",
                    "report": relative, "report_hash": best_strategy._sha256(report_path),
                }
            tree_path.write_text(json.dumps(tree))
            fake_splits = {f"holdout-{i}": "holdout" for i in range(200)}
            calls = []

            def measure(candidate, splits, persist_result, **kwargs):
                calls.append((candidate["node"], splits[0]))
                if splits[0] == "val":
                    quality = {2: .7, 3: .8, 4: .99}[candidate["node"]]
                    split = "val"
                else:
                    quality = {1: .6, 2: .85, 3: .98}[candidate["node"]]
                    split = "holdout"
                metrics = {"quality_complete": True, "completed": 200,
                           "quality": quality, "strict_passed": 100,
                           "invalid_loop": 1, "tokens": 1000, "latency_ms": 100}
                return {"status": "complete", "metrics": {
                    "best": {split: metrics}, "L0": {split: dict(metrics, quality=.5)}
                }}

            with patch.object(best_strategy, "JOB_ROOT", root.parent), \
                    patch.object(best_strategy, "_source_rows",
                                 return_value=([], fake_splits)), \
                    patch.object(best_strategy, "_artifact_version", return_value="l0-version"), \
                    patch.object(best_strategy, "run_comparison", side_effect=measure):
                chosen = best_strategy.choose_best("job")
            self.assertEqual(calls, [(2, "val"), (3, "val"),
                                     (3, "holdout"), (2, "holdout")])
            self.assertEqual(chosen["node"], 3)
            self.assertEqual(chosen["candidate_scope"]["frozen"], 3)
            self.assertEqual({row["node"] for row in chosen["showcase_candidates"]}, {1, 2, 3})

    def test_candidate_pool_fills_short_finalist_list_from_validation_only(self):
        with tempfile.TemporaryDirectory() as directory:
            root, tree_path = self._make_job(Path(directory), final_nodes=(1,))
            tree = json.loads(tree_path.read_text())
            pool = best_strategy._candidate_pool(root, tree)
            self.assertEqual(len(pool), 3)
            self.assertEqual(pool[0]["selection_source"], "frozen_final_holdout")
            self.assertEqual(sum(item["selection_source"] == "validation_fallback"
                                 for item in pool), 2)
            self.assertEqual(len({item["version"] for item in pool}), 3)

    def test_50_task_candidate_screen_uses_50_task_completion_scope(self):
        rows = []
        for index in range(50):
            row = _trial(f"val-{index}", "best", .7, "candidate-version")
            row["split"] = "val"
            rows.append(row)
        summary = best_strategy._summarize_report(
            {"complete": True, "trials": rows},
            {f"val-{i}": "val" for i in range(50)}, {"val": 50})
        self.assertEqual(summary["best"]["val"]["completed"], 50)
        self.assertTrue(summary["best"]["val"]["quality_complete"])
        self.assertFalse(summary["L0"]["val"]["quality_complete"])


if __name__ == "__main__":
    unittest.main()
