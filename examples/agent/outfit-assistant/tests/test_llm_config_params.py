#!/usr/bin/env python3
"""
Test LLM generation parameters (temperature, top_p, etc.) — default values
when unset, and skill fallback chain.

These tests verify the ``AgentConfig`` layer and the ``extra_body`` plumbing
through ``call_llm()`` → ``trace_call()`` → ``chat_completion()``.

They do NOT call a real LLM — they verify config resolution and request body
construction only.
"""

import json
import os
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "nodes"))
from common.agent_config import AgentConfig


class TestAgentConfigGenerationParams(unittest.TestCase):
    """Test that AgentConfig returns correct defaults and fallback chains."""

    def setUp(self):
        # Save and clear relevant env vars
        self._saved = {}
        for key in (
            "AF_LLM_TEMPERATURE", "AF_LLM_TOP_P",
            "AF_LLM_FREQUENCY_PENALTY", "AF_LLM_PRESENCE_PENALTY",
            "AF_LLM_TOP_K", "AF_LLM_STOP",
            "AF_SKILL_LLM_TEMPERATURE", "AF_SKILL_LLM_TOP_P",
            "AF_SKILL_LLM_FREQUENCY_PENALTY", "AF_SKILL_LLM_PRESENCE_PENALTY",
            "AF_SKILL_LLM_TOP_K", "AF_SKILL_LLM_STOP",
        ):
            self._saved[key] = os.environ.pop(key, None)

    def tearDown(self):
        for key, val in self._saved.items():
            if val is not None:
                os.environ[key] = val
            else:
                os.environ.pop(key, None)

    # ── Coordinator defaults (no env vars set) ──

    def test_llm_temperature_default(self):
        """Default temperature should be 0.7 when unset."""
        self.assertEqual(AgentConfig.llm_temperature(), 0.7)

    def test_llm_top_p_default(self):
        """Default top_p should be 1.0 when unset."""
        self.assertEqual(AgentConfig.llm_top_p(), 1.0)

    def test_llm_frequency_penalty_default(self):
        """Default frequency_penalty should be 0.0 when unset."""
        self.assertEqual(AgentConfig.llm_frequency_penalty(), 0.0)

    def test_llm_presence_penalty_default(self):
        """Default presence_penalty should be 0.0 when unset."""
        self.assertEqual(AgentConfig.llm_presence_penalty(), 0.0)

    def test_llm_top_k_default(self):
        """Default top_k should be 0 (disabled) when unset."""
        self.assertEqual(AgentConfig.llm_top_k(), 0)

    def test_llm_stop_default(self):
        """Default stop should be empty list when unset."""
        self.assertEqual(AgentConfig.llm_stop(), [])

    # ── Coordinator with env vars set ──

    def test_llm_temperature_from_env(self):
        os.environ["AF_LLM_TEMPERATURE"] = "0.5"
        self.assertEqual(AgentConfig.llm_temperature(), 0.5)

    def test_llm_top_p_from_env(self):
        os.environ["AF_LLM_TOP_P"] = "0.9"
        self.assertEqual(AgentConfig.llm_top_p(), 0.9)

    def test_llm_stop_from_env(self):
        os.environ["AF_LLM_STOP"] = "\n,\n\n"
        self.assertEqual(AgentConfig.llm_stop(), [])

    def test_llm_stop_multiple(self):
        os.environ["AF_LLM_STOP"] = "stop1,stop2"
        self.assertEqual(AgentConfig.llm_stop(), ["stop1", "stop2"])

    # ── Skill executor defaults (no env vars set, no coordinator fallback) ──

    def test_skill_llm_temperature_default(self):
        """Skill temperature should fallback to coordinator default (0.7)."""
        self.assertEqual(AgentConfig.skill_llm_temperature(), 0.7)

    def test_skill_llm_temperature_override(self):
        """Skill temperature should use AF_SKILL_LLM_TEMPERATURE when set."""
        os.environ["AF_SKILL_LLM_TEMPERATURE"] = "0.3"
        self.assertEqual(AgentConfig.skill_llm_temperature(), 0.3)

    # ── Skill fallback chain: AF_SKILL_* → AF_LLM_* → code default ──

    def test_skill_llm_temperature_fallback_to_coordinator(self):
        """Skill temperature should fallback to AF_LLM_TEMPERATURE when AF_SKILL_* unset."""
        os.environ["AF_LLM_TEMPERATURE"] = "0.9"
        # Clear skill var to ensure fallback
        os.environ.pop("AF_SKILL_LLM_TEMPERATURE", None)
        self.assertEqual(AgentConfig.skill_llm_temperature(), 0.9)

    def test_skill_llm_top_p_fallback_to_coordinator(self):
        """Skill top_p should fallback to AF_LLM_TOP_P when AF_SKILL_* unset."""
        os.environ["AF_LLM_TOP_P"] = "0.8"
        os.environ.pop("AF_SKILL_LLM_TOP_P", None)
        self.assertEqual(AgentConfig.skill_llm_top_p(), 0.8)

    def test_skill_llm_stop_fallback_to_coordinator(self):
        """Skill stop should fallback to AF_LLM_STOP when AF_SKILL_* unset."""
        os.environ["AF_LLM_STOP"] = "end,quit"
        os.environ.pop("AF_SKILL_LLM_STOP", None)
        self.assertEqual(AgentConfig.skill_llm_stop(), ["end", "quit"])

    # ── extra_body construction (None values are filtered by chat_completion) ──

    def test_extra_body_temperature_top_p_not_none(self):
        """temperature and top_p should always have a valid float value."""
        for getter, name in [
            (AgentConfig.llm_temperature, "temperature"),
            (AgentConfig.llm_top_p, "top_p"),
            (AgentConfig.llm_frequency_penalty, "frequency_penalty"),
            (AgentConfig.llm_presence_penalty, "presence_penalty"),
        ]:
            val = getter()
            self.assertIsNotNone(
                val,
                f"coordinator '{name}' should not be None — "
                f"default must be a valid float"
            )

    def test_extra_body_top_k_zero_is_none(self):
        """top_k=0 means disabled, converted to None in extra_body construction."""
        val = AgentConfig.llm_top_k()  # 0
        self.assertEqual(val, 0)
        self.assertIsNone(val or None)  # matches the coordinator code pattern

    def test_extra_body_stop_empty_is_none(self):
        """empty stop list is converted to None in extra_body construction."""
        val = AgentConfig.llm_stop()  # []
        self.assertEqual(val, [])
        self.assertIsNone(val or None)  # matches the coordinator code pattern

    def test_extra_body_values_in_range(self):
        """temperature, top_p, frequency_penalty, presence_penalty must be in valid range."""
        for getter, name in [
            (AgentConfig.llm_temperature, "temperature"),
            (AgentConfig.llm_top_p, "top_p"),
            (AgentConfig.llm_frequency_penalty, "frequency_penalty"),
            (AgentConfig.llm_presence_penalty, "presence_penalty"),
        ]:
            val = getter()
            self.assertGreaterEqual(val, 0.0,
                                    f"{name} should be >= 0.0, got {val}")
            self.assertLessEqual(val, 2.0,
                                 f"{name} should be <= 2.0, got {val}")


if __name__ == "__main__":
    unittest.main()