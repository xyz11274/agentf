"""Pure local fixtures; no models, processes, or report mutations."""
import copy
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location("evaluation_cost", Path(__file__).with_name("evaluation_cost.py"))
m = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(m)


def trial(identity, task="task", variant="candidate", split="train", duration=10, tokens=100,
          calls=2, run=None):
    return {"trial_id": identity, "task_id": task, "task_version": "v1", "repetition": 0,
            "variant_id": variant, "split": split, "duration_ms": duration,
            "metrics": {"tokens": tokens, "llm_calls": calls, "quality": .5},
            "evidence": {"execution_run_id": run or identity}}


class AgentCostTests(unittest.TestCase):
    def test_execution_retry_counts_and_judge_only_retry_does_not(self):
        first = {"trials": [trial("failed"), trial("task2", "task2")]}
        retry = {"trials": [trial("replacement", duration=20, tokens=200, calls=3), trial("task2", "task2")]}
        rejudge = copy.deepcopy(retry)
        rejudge["trials"][0]["metrics"]["quality"] = 1
        source = copy.deepcopy([first, retry, rejudge])
        result = m.aggregate_agent_cost([first, retry, rejudge])
        group = result["groups"][0]
        self.assertEqual(group["unique_trial_ids"], 3)
        self.assertEqual(group["agent_executions"], 3)
        self.assertEqual(group["planned_trials"], 2)
        self.assertEqual(group["extra_executions"], 1)
        self.assertEqual(group["totals"], {"duration_ms": 40, "tokens": 400, "llm_calls": 7})
        self.assertEqual(group["mean_per_planned_trial_including_retries"]["duration_ms"], 20)
        self.assertNotIn("quality", json.dumps(result))
        self.assertEqual([first, retry, rejudge], source)

    def test_same_known_trace_with_different_judge_trial_id_counts_once(self):
        result = m.aggregate_agent_cost([{"trials": [trial("first", run="agent-run"),
                                                  trial("rejudge", run="agent-run")]}])
        group = result["groups"][0]
        self.assertEqual((group["unique_trial_ids"], group["agent_executions"]), (2, 1))
        self.assertEqual(group["extra_executions"], 0)
        self.assertEqual(group["totals"]["tokens"], 100)

    def test_variant_split_separation_and_explicit_planned_denominator(self):
        result = m.aggregate_agent_cost([{"trials": [trial("a", variant="L0", split="val"),
            trial("b", variant="best", split="val"), trial("c", variant="best", split="holdout")]}],
            {("best", "val"): 200})
        self.assertEqual(len(result["groups"]), 3)
        val = next(g for g in result["groups"] if g["variant_id"] == "best" and g["split"] == "val")
        self.assertEqual(val["planned_trials"], 200)
        self.assertEqual(val["mean_per_planned_trial_including_retries"]["tokens"], .5)

    def test_missing_costs_are_disclosed_and_repeated_observations_fill_them(self):
        partial = trial("a", tokens=None)
        result = m.aggregate_agent_cost([{"trials": [partial]}])["groups"][0]
        self.assertFalse(result["cost_complete"])
        self.assertEqual(result["missing_cost_executions"]["tokens"], 1)
        full = m.aggregate_agent_cost([{"trials": [partial, trial("a")]}])["groups"][0]
        self.assertTrue(full["cost_complete"])
        self.assertEqual(full["totals"]["tokens"], 100)

    def test_reused_trial_id_with_different_execution_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "conflicting_trial_identity"):
            m.aggregate_agent_cost([{"trials": [trial("a", run="run1"), trial("a", run="run2")]}])

    def test_cli_sidecar_and_input_overwrite_guard(self):
        with tempfile.TemporaryDirectory() as tmp:
            report = Path(tmp) / "report.json"
            report.write_text(json.dumps({"trials": [trial("a")]}))
            before = report.read_bytes()
            out = Path(tmp) / "cost.json"
            self.assertEqual(m.main([str(report), "--output", str(out)]), 0)
            self.assertEqual(json.loads(out.read_text())["scope"], "agent_execution_only")
            with self.assertRaises(SystemExit):
                m.main([str(report), "--output", str(report)])
            self.assertEqual(report.read_bytes(), before)


if __name__ == "__main__":
    unittest.main()
