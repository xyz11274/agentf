#!/usr/bin/env python3
"""Unit tests for the harness decision core (decide_next) and guards.

These tests exercise the harness loop semantics — loop / guards / termination —
directly against the pure functions in ``harness_lib``, with no AgentFlow
framework involved. They are deterministic and offline (no LLM).

Run:  python3 -m unittest discover -s tests/unit
"""
import json
import os
import sys
import unittest
from pathlib import Path

# Make the package node dir importable.
PKG_NODES = Path(__file__).resolve().parents[2] / "package" / "nodes" / "python"
sys.path.insert(0, str(PKG_NODES))

import harness_lib as hl  # noqa: E402


CATALOG = [
    {"name": "retrieve", "title": "检索", "expected_parameters": ["user_text"]},
    {"name": "analyze", "title": "分析", "expected_parameters": ["user_text"]},
    {"name": "compose", "title": "综合", "expected_parameters": ["user_text"]},
    {"name": "finalize", "title": "定稿", "expected_parameters": ["user_text"]},
]

CONFIG = {
    "max_iterations": 6,
    "max_observation_length": 50,
    "stuck_repeat_threshold": 2,
    "skill_order": ["retrieve", "analyze", "compose", "finalize"],
}

AVAIL = {"user_text": "请规划一次旅行"}


def env(skill, status="ok", result="r", can_handle=True):
    return hl.make_envelope(skill, status=status, result=result, can_handle=can_handle)


class LoopStateMachineTests(unittest.TestCase):
    def test_empty_results_selects_retrieve_first(self):
        out = hl.decide_next(hl.new_state(), CATALOG, {}, CONFIG, AVAIL)
        self.assertEqual(out["next_skills"], ["retrieve"])
        self.assertEqual(out["terminated_reason"], "")

    def test_progression_is_result_driven(self):
        results = {}
        order_seen = []
        state = hl.new_state()
        for _ in range(4):
            out = hl.decide_next(state, CATALOG, results, CONFIG, AVAIL)
            state = out["state"]
            if not out["next_skills"]:
                break
            skill = out["next_skills"][0]
            order_seen.append(skill)
            results[skill] = env(skill, result=f"{skill}-done")
        self.assertEqual(order_seen, ["retrieve", "analyze", "compose", "finalize"])

    def test_one_skill_per_iteration(self):
        out = hl.decide_next(hl.new_state(), CATALOG, {"retrieve": env("retrieve")},
                             CONFIG, AVAIL)
        self.assertEqual(len(out["next_skills"]), 1)

    def test_next_depends_on_accumulated_not_fixed_sequence(self):
        # If analyze result is injected out of order, retrieve is still the gap.
        results = {"analyze": env("analyze")}
        out = hl.decide_next(hl.new_state(), CATALOG, results, CONFIG, AVAIL)
        self.assertEqual(out["next_skills"], ["retrieve"])


class TerminationTests(unittest.TestCase):
    def test_terminates_with_response_when_finalized(self):
        results = {
            "retrieve": env("retrieve", result="facts"),
            "analyze": env("analyze", result="insights"),
            "compose": env("compose", result="draft"),
            "finalize": env("finalize", result="FINAL ANSWER"),
        }
        out = hl.decide_next(hl.new_state(), CATALOG, results, CONFIG, AVAIL)
        self.assertEqual(out["next_skills"], [])
        self.assertEqual(out["terminated_reason"], "completed")
        self.assertEqual(out["response"], "FINAL ANSWER")

    def test_finalize_runs_before_completion(self):
        results = {
            "retrieve": env("retrieve"),
            "analyze": env("analyze"),
            "compose": env("compose"),
        }
        out = hl.decide_next(hl.new_state(), CATALOG, results, CONFIG, AVAIL)
        self.assertEqual(out["next_skills"], ["finalize"])


class GuardTests(unittest.TestCase):
    def test_max_iterations_emits_fallback_response(self):
        state = hl.new_state()
        state["iteration"] = CONFIG["max_iterations"]  # next call exceeds
        results = {"retrieve": env("retrieve", result="partial facts")}
        out = hl.decide_next(state, CATALOG, results, CONFIG, AVAIL)
        self.assertEqual(out["next_skills"], [])
        self.assertEqual(out["terminated_reason"], "max_iterations")
        self.assertTrue(out["response"], "fallback response must be non-empty")

    def test_stuck_detection_converges(self):
        results = {"retrieve": env("retrieve")}
        state = hl.new_state()
        # First decision establishes the signature and would pick analyze.
        out1 = hl.decide_next(state, CATALOG, results, CONFIG, AVAIL)
        state = out1["state"]
        # Re-decide repeatedly with identical results -> stuck.
        reason = ""
        for _ in range(CONFIG["stuck_repeat_threshold"] + 1):
            out = hl.decide_next(state, CATALOG, results, CONFIG, AVAIL)
            state = out["state"]
            reason = out["terminated_reason"]
            if reason:
                break
        self.assertEqual(reason, "stuck")

    def test_observation_truncation_preserves_json(self):
        long_result = "x" * 5000
        e = hl.make_envelope("retrieve", result=long_result)
        truncated = hl.truncate_observation(e, 100)
        # Outer envelope still serializes/parses cleanly.
        parsed = json.loads(json.dumps(truncated, ensure_ascii=False))
        self.assertTrue(parsed["truncated"])
        self.assertLessEqual(len(parsed["result"]), 100 + len("...[truncated]"))
        self.assertEqual(parsed["skill"], "retrieve")


class ParameterAndFailureTests(unittest.TestCase):
    def test_missing_expected_parameters_skips_and_advances(self):
        # retrieve requires user_text; provide none -> it is skipped (unsupported)
        # and the loop advances toward analyze instead of stalling.
        out = hl.decide_next(hl.new_state(), CATALOG, {}, CONFIG, available_parameters={})
        self.assertNotIn("retrieve", out.get("next_skills", []))

    def test_error_status_still_converges(self):
        results = {
            "retrieve": env("retrieve", status="error", result="", can_handle=False),
            "analyze": env("analyze", result="insights"),
            "compose": env("compose", result="draft"),
            "finalize": env("finalize", result="DONE"),
        }
        out = hl.decide_next(hl.new_state(), CATALOG, results, CONFIG, AVAIL)
        self.assertEqual(out["next_skills"], [])
        self.assertEqual(out["terminated_reason"], "completed")


class ConfigCustomizationTests(unittest.TestCase):
    def test_max_iterations_is_config_driven(self):
        cfg = dict(CONFIG, max_iterations=1)
        state = hl.new_state()
        state["iteration"] = 1
        out = hl.decide_next(state, CATALOG, {"retrieve": env("retrieve")}, cfg, AVAIL)
        self.assertEqual(out["terminated_reason"], "max_iterations")

    def test_skill_order_is_config_driven(self):
        cfg = dict(CONFIG, skill_order=["analyze", "retrieve", "finalize"])
        out = hl.decide_next(hl.new_state(), CATALOG, {}, cfg, AVAIL)
        self.assertEqual(out["next_skills"], ["analyze"])


class DialogueIsolationTests(unittest.TestCase):
    def test_independent_states_do_not_interfere(self):
        # Two dialogues tracked by separate state dicts advance independently.
        s_a, s_b = hl.new_state(), hl.new_state()
        res_a, res_b = {}, {"retrieve": env("retrieve")}
        out_a = hl.decide_next(s_a, CATALOG, res_a, CONFIG, AVAIL)
        out_b = hl.decide_next(s_b, CATALOG, res_b, CONFIG, AVAIL)
        self.assertEqual(out_a["next_skills"], ["retrieve"])
        self.assertEqual(out_b["next_skills"], ["analyze"])
        self.assertNotEqual(out_a["state"]["last_observation_hash"],
                            out_b["state"]["last_observation_hash"])


class PromptFillTests(unittest.TestCase):
    def test_fill_prompt_replaces_and_clears(self):
        tpl = "User: {{user_text}}\nContext: {{ctx}}\nLeft: {{unknown}}"
        out = hl.fill_prompt(tpl, {"user_text": "hi", "ctx": "c"})
        self.assertIn("User: hi", out)
        self.assertIn("Context: c", out)
        self.assertNotIn("{{", out)


if __name__ == "__main__":
    unittest.main(verbosity=2)
