"""BFCL terminal output must be identical in trace and evaluation response."""
from copy import deepcopy
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "experiments/bfcl-v3"))
from bfcl_v3_host import Trial


def close_turn(level="L3", final=True, receipts=None, text="answer"):
    trial = Trial.__new__(Trial)
    trial.run_context = {"run_id": "run-1"}
    trial.level = level
    trial.policy_applied = {"L1": 0, "L2": 0, "L3": 0}
    trial.external_error_code = ""
    events = []
    def counts(context):
        assert context == {"run_id": "run-1"}
        assert not events, "Read receipts before sealing the run output"
        return receipts or {}
    trial._receipt_counts = counts
    trial._point = lambda fields: events.append(deepcopy(fields)) or {}
    output = trial._close_turn({"response": text, "l1_applied": 0, "l2_applied": 0,
                                "l3_applied": 0, "bfcl_valid": True}, final=final)
    assert events[0]["output"] == output
    assert trial.run_context is None
    return output


def test_applied_receipts_are_frozen_before_run_end():
    assert close_turn(receipts={"applied": 3})["l3_applied"] == 3


def test_l0_cannot_gain_evolution_applications():
    result = close_turn(level="L0", receipts={"applied": 3})
    assert result["l1_applied"] == result["l2_applied"] == result["l3_applied"] == 0


def test_missing_receipts_preserve_existing_output():
    assert close_turn(receipts={})["l3_applied"] == 0


def test_nonfinal_turn_does_not_request_final_counts():
    assert close_turn(final=False, receipts={"applied": 3})["l3_applied"] == 0


def test_redaction_is_identical_in_trace_and_response():
    assert close_turn(text="ark-" + "x" * 20)["response"] == "<REDACTED>"
