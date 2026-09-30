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


def _fake_artifact_version(artifact):
    if artifact == {"node": "l0"}:
        return "l0-version"
    if isinstance(artifact, dict) and isinstance(artifact.get("node"), int):
        return f"v{artifact['node']}"
    return "v-ext"


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
    def test_more_strict_completions_rank_before_partial_quality(self):
        self.assertGreater(best_strategy._showcase_key({'strict_passed': 21, 'quality': .50}),
                           best_strategy._showcase_key({'strict_passed': 20, 'quality': .70}))
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

    def test_choose_best_uses_frozen_validation_and_holdout_is_audit_only(self):
        with tempfile.TemporaryDirectory() as directory:
            root, _ = self._make_job(Path(directory))
            fake_splits = {f"holdout-{i}": "holdout" for i in range(200)}
            with patch.object(best_strategy, "JOB_ROOT", root.parent), \
                    patch.object(best_strategy, "OUTPUT", Path(directory) / "output"), \
                    patch.object(best_strategy, "_source_rows",
                                 return_value=([], fake_splits)), \
                    patch.object(best_strategy, "_artifact_version", side_effect=_fake_artifact_version):
                chosen = best_strategy.choose_best("job")
            # Node 4 is outside the frozen three. Within the frozen pool,
            # node 3 has the best validation although node 2 wins holdout.
            self.assertEqual(chosen["node"], 3)
            self.assertEqual(chosen["selection_reason"],
                             "best_of_frozen_candidates_by_shared_validation_only")
            self.assertEqual(chosen["best_selection_basis"],
                             "shared_val_evidence_before_holdout")
            self.assertTrue(chosen["showcase_only"])
            self.assertFalse(chosen["formal_gate_passed"])
            self.assertEqual(chosen["holdout"]["completed"], 200)
            self.assertEqual(chosen["l0_holdout"]["completed"], 200)
            self.assertEqual(chosen["showcase_candidate_count"], 3)
            self.assertEqual({row["node"] for row in chosen["showcase_candidates"]}, {1, 2, 3})
            freeze = Path(directory) / "output" / chosen["selection_freeze_path"]
            self.assertTrue(freeze.is_file())
            frozen = json.loads(freeze.read_text())
            self.assertEqual(frozen["best_node"], 3)
            frozen_bytes = freeze.read_bytes()
            tree_path = root / "search/tree.json"
            changed_tree = json.loads(tree_path.read_text())
            changed_tree["nodes"][1]["validation"] = {
                key: dict(value, quality=1.0)
                for key, value in changed_tree["nodes"][1]["validation"].items()
            }
            tree_path.write_text(json.dumps(changed_tree))
            with patch.object(best_strategy, "JOB_ROOT", root.parent), \
                    patch.object(best_strategy, "OUTPUT", Path(directory) / "output"), \
                    patch.object(best_strategy, "_source_rows",
                                 return_value=([], fake_splits)), \
                    patch.object(best_strategy, "_artifact_version", side_effect=_fake_artifact_version):
                resumed = best_strategy.choose_best("job")
            self.assertEqual(resumed["node"], 3)
            self.assertEqual(freeze.read_bytes(), frozen_bytes)

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
                    patch.object(best_strategy, "OUTPUT", Path(directory) / "output"), \
                    patch.object(best_strategy, "_source_rows",
                                 return_value=([], fake_splits)), \
                    patch.object(best_strategy, "_artifact_version", side_effect=_fake_artifact_version):
                chosen = best_strategy.choose_best("job")
            self.assertEqual(chosen["node"], 1)
            self.assertEqual(chosen["selection_reason"],
                             "best_of_frozen_candidates_by_shared_validation_only")
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
                    patch.object(best_strategy, "_artifact_version", side_effect=_fake_artifact_version):
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
                    patch.object(best_strategy, "OUTPUT", Path(directory) / "output"), \
                    patch.object(best_strategy, "_source_rows",
                                 return_value=([], fake_splits)), \
                    patch.object(best_strategy, "_artifact_version", side_effect=_fake_artifact_version), \
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

    def test_candidate_pool_uses_current_shared_val50_scope(self):
        with tempfile.TemporaryDirectory() as directory:
            root, tree_path = self._make_job(Path(directory))
            tree = json.loads(tree_path.read_text())
            for node in tree["nodes"]:
                node["validation"] = dict(list(node["validation"].items())[:50])
            tree_path.write_text(json.dumps(tree))
            pool = best_strategy._candidate_pool(root, tree)
            self.assertEqual(len(pool), 3)
            self.assertEqual({item["validation"]["completed"] for item in pool}, {50})

    def test_l0_cache_reuses_explicit_exact_val50_regrade(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            root = base / "job"
            root.mkdir()
            (root / "baseline.json").write_text(json.dumps({"artifact": "real-l0"}))
            task_ids = [f"val-{index}" for index in range(50)]
            version = "agent-version-v1:l0"
            rows = []
            for task_id in task_ids:
                row = _trial(task_id, "candidate", .7, version)
                row["split"] = "val"
                rows.append(row)
            report_path = base / "output/audit/val-regrade-20260929/l0-split/retry-1/report.json"
            report_path.parent.mkdir(parents=True)
            report_path.write_text(json.dumps({
                "complete": True, "dataset_version": "dataset-test",
                "configuration": {"variants": [{"id": "candidate", "version": version}]},
                "trials": rows,
            }))
            tree = {"dataset_version": "dataset-test", "nodes": [{
                "validation": {task_id: {} for task_id in task_ids}
            }]}
            with patch.object(best_strategy, "OUTPUT", base / "output"):
                cached = best_strategy._cached_val_report(root, tree, version, task_ids)
            self.assertIsNotNone(cached)
            self.assertEqual(len(cached[0]), 50)
            self.assertEqual(cached[1], "audit/val-regrade-20260929/l0-split/retry-1/report.json")

    def test_source_val_audit_maps_only_through_hash_frozen_id_view(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            config = root / "config"
            runtime = config / "gepa-deployment/runtime"
            runtime.mkdir(parents=True)
            source = config / "source"
            source.mkdir()
            ids = [f"val-{index}" for index in range(50)]
            source_version, view_version = "source-v1", "view-v1"
            (source / "manifest.json").write_text(json.dumps({
                "metadata": {"dataset_version": source_version}
            }))
            view = {"source": "source", "source_version": source_version,
                    "selection_task_ids": ids}
            view_path = config / "gepa-view.json"
            view_path.write_text(json.dumps(view))
            plan = {"source_dataset_version": source_version,
                    "dataset_version": view_version,
                    "dataset_view": str(view_path.relative_to(root)),
                    "dataset_view_hash": best_strategy._sha256(view_path)}
            (config / "gepa-deployment/plan.json").write_text(json.dumps(plan))
            with patch.object(best_strategy, "ROOT", root), \
                    patch.object(best_strategy, "SOURCE", source), \
                    patch.object(best_strategy, "GEPA_RUNTIME", runtime):
                self.assertTrue(best_strategy._compatible_validation_dataset(
                    root, view_version, source_version, ids))
                view["goal_contracts_file"] = "contracts.json"
                view_path.write_text(json.dumps(view))
                plan["dataset_view_hash"] = best_strategy._sha256(view_path)
                (config / "gepa-deployment/plan.json").write_text(json.dumps(plan))
                self.assertFalse(best_strategy._compatible_validation_dataset(
                    root, view_version, source_version, ids))

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

    def test_external_seed_registry_preserves_string_id_without_fake_node(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            artifact_path = root / "seed.json"
            artifact_path.write_text(json.dumps({"snapshot": {"experiences": []}}))
            report_path = root / "val50.json"
            report_path.write_text(json.dumps({
                "trials": [{"task_id": f"val-{i}", "variant_id": "best", "split": "val"}
                           for i in range(50)]
            }))
            registry_path = root / "registry.json"
            registry_path.write_text(json.dumps({"job_id": "job", "candidates": [{
                "id": "seed96-clean", "version": "v-clean",
                "artifact_file": str(artifact_path), "val50_report": str(report_path),
                "variant_id": "best",
            }]}))
            with patch.object(best_strategy, "ROOT", root), \
                    patch.object(best_strategy, "EXTERNAL_SEED_REGISTRY", registry_path):
                candidates = best_strategy.discover_external_seed_candidates("job")
            self.assertEqual(len(candidates), 1)
            self.assertEqual(candidates[0]["external_candidate_id"], "seed96-clean")
            self.assertIsNone(candidates[0]["node"])
            self.assertEqual(candidates[0]["screening_variant_id"], "best")
            self.assertEqual(len(candidates[0]["screening_task_ids"]), 50)

    def test_external_seed_requires_complete_exact_val50_before_selection(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            job_root = base / "job"
            (job_root / "search").mkdir(parents=True)
            ids = [f"task-{i}" for i in range(50)]
            (job_root / "search/tree.json").write_text(json.dumps({"nodes": [{
                "validation": {task_id: {} for task_id in ids}
            }]}))
            artifact = {"snapshot": {"experiences": []}}
            report_path = base / "external-val50.json"
            report_path.write_text(json.dumps({
                "dataset_version": "dataset-test",
                "configuration": {"variants": [{"id": "best", "version": "v-ext"}]},
                "trials": [{"task_id": task_id, "variant_id": "best", "split": "val",
                            "evidence": {"loaded_version": "v-ext"}}
                           for task_id in ids],
            }))
            candidate = {
                "job_id": "job", "node": None, "external_candidate_id": "seed96-clean",
                "selection_source": "external_seed_registry", "version": "v-ext",
                "artifact": artifact, "screening_task_ids": ids,
                "screening_report_path": str(report_path), "screening_variant_id": "best",
            }
            with patch.object(best_strategy, "JOB_ROOT", base), \
                    patch.object(best_strategy, "_expected_dataset_version", return_value="dataset-test"), \
                    patch.object(best_strategy, "_compatible_validation_dataset", return_value=True), \
                    patch.object(best_strategy, "_artifact_version", return_value="v-ext"), \
                    patch.object(best_strategy, "_aggregate", return_value={
                        "quality_complete": True, "completed": 50, "scored": 50, "errors": 0,
                        "quality": .61, "strict_passed": 22,
                    }):
                accepted = best_strategy._independent_val_candidates(
                    "job", [candidate], set(ids), repair_errors=False)
            self.assertEqual(len(accepted), 1)
            self.assertIsNone(accepted[0]["node"])
            self.assertEqual(accepted[0]["external_candidate_id"], "seed96-clean")
            self.assertEqual(accepted[0]["selection_source"], "external_seed_registry")
            self.assertEqual(accepted[0]["validation"]["completed"], 50)

    def test_external_candidate_artifact_is_frozen_with_source_receipt(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            artifact = {"snapshot": {"experiences": [{"id": "memory-x"}]}}
            candidate = {"node": None, "external_candidate_id": "seed96-clean",
                         "version": "v-ext", "artifact": artifact,
                         "artifact_source_path": "trace_output/audit/seed.json"}
            with patch.object(best_strategy, "ROOT", root), \
                    patch.object(best_strategy, "_artifact_version", return_value="v-ext"):
                receipt = best_strategy._freeze_candidate_artifact(
                    root, root / "freeze", candidate)
            saved = root / receipt["artifact_path"]
            self.assertTrue(saved.is_file())
            self.assertEqual(json.loads(saved.read_text()), artifact)
            self.assertEqual(receipt["artifact_source_path"], candidate["artifact_source_path"])

    def test_comparison_complete_requires_every_trial_scored_without_errors(self):
        report = {"complete": True}
        complete = {"best": {"val": {"quality_complete": True, "completed": 50,
                                       "scored": 50, "errors": 0}}}
        self.assertTrue(best_strategy._comparison_is_complete(
            report, complete, {"val": 50}, include_l0=False))
        failed_judge = {"best": {"val": {"quality_complete": False, "completed": 50,
                                           "scored": 45, "errors": 5}}}
        self.assertFalse(best_strategy._comparison_is_complete(
            report, failed_judge, {"val": 50}, include_l0=False))
        incomplete_l0 = {
            "best": {"val": {"quality_complete": True, "completed": 50,
                              "scored": 50, "errors": 0}},
            "L0": {"val": {"quality_complete": False, "completed": 50,
                            "scored": 49, "errors": 1}},
        }
        self.assertFalse(best_strategy._comparison_is_complete(
            report, incomplete_l0, {"val": 50}, include_l0=True))


if __name__ == "__main__":
    unittest.main()
