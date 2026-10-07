"""Shared, model-free acceptance rules for every dataset adapter.

The caller supplies already paired metrics from the same task scope.  This
module deliberately does no I/O and never changes scoring; it only applies the
release contract consistently to business and open datasets.
"""
from __future__ import annotations

import math
from typing import Any


RESOURCE_FIELDS = ("latency_ms", "tokens", "model_calls")


def _num(value: Any) -> float | None:
    if isinstance(value, (int, float)) and not isinstance(value, bool):
        value = float(value)
        return value if math.isfinite(value) else None
    return None


def relative_gain(candidate: Any, baseline: Any) -> float | None:
    candidate, baseline = _num(candidate), _num(baseline)
    if candidate is None or baseline is None or baseline == 0:
        return None
    return (candidate - baseline) / abs(baseline)


def evaluate(baseline: dict[str, Any], candidate: dict[str, Any], *,
             baseline_loop_by_task: dict[str, Any] | None = None,
             candidate_loop_by_task: dict[str, Any] | None = None,
             baseline_loop_basis: Any = None,
             candidate_loop_basis: Any = None,
             same_scope: bool = True,
             complete: bool = True,
             require_resources: bool = True,
             require_intent_when_measured: bool = False) -> dict[str, Any]:
    """Evaluate the common quality/strict/loop/resource contract.

    Loop reduction is blocked (never treated as zero) when the baseline is
    zero, coverage is incomplete, or formal bases differ. Intent is optional:
    it is checked only when both sides provide measurements on this scope.
    """
    quality_gain = relative_gain(candidate.get("quality"), baseline.get("quality"))
    strict_gain = relative_gain(candidate.get("strict_passed"), baseline.get("strict_passed"))
    quality_ok = quality_gain is not None and quality_gain >= .05 - 1e-12
    strict_ok = strict_gain is not None and strict_gain >= .05 - 1e-12

    b_loops = baseline_loop_by_task or {}
    c_loops = candidate_loop_by_task or {}
    paired = set(b_loops) & set(c_loops)
    loop_complete = (same_scope and bool(paired) and set(b_loops) == set(c_loops)
                     and all(_num(b_loops[k]) is not None and _num(c_loops[k]) is not None
                             for k in paired))
    same_basis = bool(baseline_loop_basis and candidate_loop_basis
                      and baseline_loop_basis == candidate_loop_basis)
    b_loop = _num(baseline.get("avg_invalid_loop"))
    c_loop = _num(candidate.get("avg_invalid_loop"))
    if loop_complete and same_basis and b_loop is not None and c_loop is not None and b_loop != 0:
        loop_gain = (b_loop - c_loop) / abs(b_loop)
        loop_ok = loop_gain >= .15 - 1e-12
        loop_state = "pass" if loop_ok else "fail"
    else:
        loop_gain, loop_ok, loop_state = None, False, "blocked_unverifiable"

    resources: dict[str, bool] = {}
    for field in RESOURCE_FIELDS:
        left, right = _num(candidate.get(field)), _num(baseline.get(field))
        resources[field] = left is not None and right is not None and left <= right
    resource_complete = all(_num(candidate.get(f)) is not None and _num(baseline.get(f)) is not None
                            for f in RESOURCE_FIELDS)
    resources_ok = resource_complete and all(resources.values())

    b_intent, c_intent = _num(baseline.get("intent_f1")), _num(candidate.get("intent_f1"))
    intent_measured = b_intent is not None and c_intent is not None and same_scope
    intent_ok = (c_intent >= b_intent) if intent_measured else not require_intent_when_measured
    checks = {
        "quality_relative_gain_5pct": quality_ok,
        "strict_relative_gain_5pct": strict_ok,
        "invalid_loop_reduction_15pct": loop_ok,
        "resources_not_increased": resources_ok,
    }
    if intent_measured:
        checks["intent_not_degraded"] = intent_ok
    required_checks = [key for key in checks
                       if require_resources or key != "resources_not_increased"]
    return {
        "checks": checks,
        "required_checks": required_checks,
        "resources_required": require_resources,
        "passed": bool(complete and all(checks[key] for key in required_checks)),
        "quality_gain": quality_gain, "strict_gain": strict_gain,
        "loop_gain": loop_gain, "loop_state": loop_state,
        "loop_complete": loop_complete, "loop_basis_match": same_basis,
        "paired_loop_coverage": len(paired),
        "resources": resources, "resource_complete": resource_complete,
        "resource_state": "pass" if resources_ok else ("blocked_unverifiable" if not resource_complete else "fail"),
        "intent_measured": intent_measured, "intent_ok": intent_ok,
    }


def gate(baseline: dict[str, Any], candidate: dict[str, Any], **kwargs: Any) -> dict[str, Any]:
    """Alias with an explicit name for adapter gate callers."""
    return evaluate(baseline, candidate, **kwargs)
