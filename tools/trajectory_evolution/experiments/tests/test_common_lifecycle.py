"""Contract tests for the shared lifecycle controller.

These tests use no model, AgentFlow process, network, or bwrap.  They prove
only stage ordering, gate blocking, and resumable state shape; real dataset
adapters remain responsible for native execution and scoring.
"""
from __future__ import annotations

from pathlib import Path
import json
import sys

import pytest

sys.path.insert(0, str(Path(__file__).parents[1]))

from common_lifecycle import (  # noqa: E402
    LifecycleController,
    STAGE_NAMES,
    StageResult,
)


class FakeAdapter:
    name = "fake"
    output_root = Path("/tmp/fake-lifecycle")

    def split_counts(self):
        return {"train": 1, "val": 1, "holdout": 1}

    def materialize(self, view):  # pragma: no cover - interface smoke only
        return {"view": view}

    def baseline(self, scope):  # pragma: no cover - interface smoke only
        return {"scope": scope}


def test_full_order_and_state_shape(tmp_path):
    calls = []

    def runner(stage, context):
        calls.append((stage, context["dataset"]["name"]))
        return StageResult(
            "complete",
            metrics={"completed": 1},
            gate_passed=True if stage in {"pilot32", "validation100", "comparison500"} else None,
            artifact=f"artifact:{stage}",
        )

    state = LifecycleController(FakeAdapter(), runner, tmp_path / "state.json").run()
    assert [stage for stage, _ in calls] == list(STAGE_NAMES)
    assert state["status"] == "published"
    assert state["schema_version"] == 1
    assert state["dataset"]["split_counts"] == {"train": 1, "val": 1, "holdout": 1}
    assert state["artifacts"]["publish"] == "artifact:publish"
    assert state["metrics"]["train"] == {"completed": 1}


def test_gate_failure_blocks_at_failing_stage(tmp_path):
    calls = []

    def runner(stage, context):
        calls.append(stage)
        # Evidence can be complete while the required goal gate is false.
        return {"status": "complete", "gate_passed": False if stage == "pilot32" else True}

    state = LifecycleController(FakeAdapter(), runner, tmp_path / "state.json").run()
    assert calls == ["prepare", "train", "seed", "pilot32"]
    assert state["status"] == "blocked"
    assert state["phase"] == "pilot32"
    assert state["last_error"] == "stage_gate_not_passed:pilot32"


def test_resume_reuses_completed_stages_without_fingerprint_gate(tmp_path):
    calls = []
    path = tmp_path / "state.json"

    def first(stage, context):
        calls.append(stage)
        return {"status": "complete", "gate_passed": True}

    LifecycleController(FakeAdapter(), first, path).run()
    calls.clear()

    def resumed(stage, context):
        calls.append(stage)
        return {"status": "complete", "gate_passed": True}

    # The second run does not inspect code/config fingerprints.  Its only
    # resume boundary is the durable completed-stage record.
    state = LifecycleController(FakeAdapter(), resumed, path).run()
    assert calls == []
    assert state["status"] == "published"


def test_stop_at_runs_only_a_dependency_prefix(tmp_path):
    calls = []

    def runner(stage, context):
        calls.append(stage)
        return {"status": "complete", "gate_passed": True}

    controller = LifecycleController(FakeAdapter(), runner, tmp_path / "state.json")
    state = controller.run(stop_at="seed")
    assert calls == ["prepare", "train", "seed"]
    assert state["status"] == "complete"
    assert state["phase"] == "seed"

    state = controller.run(stop_at="validation100")
    assert calls == ["prepare", "train", "seed", "pilot32", "validation100"]
    assert state["status"] == "complete"
    assert state["phase"] == "validation100"


def test_stop_at_gate_failure_is_blocked_at_stage(tmp_path):
    calls = []

    def runner(stage, context):
        calls.append(stage)
        return {"status": "complete", "gate_passed": stage != "pilot32"}

    state = LifecycleController(FakeAdapter(), runner, tmp_path / "state.json").run(
        stop_at="pilot32"
    )
    assert calls == ["prepare", "train", "seed", "pilot32"]
    assert state["status"] == "blocked"
    assert state["phase"] == "pilot32"
    assert state["last_error"] == "stage_gate_not_passed:pilot32"
    assert state["stage_results"]["pilot32"]["status"] == "complete"
    assert state["stage_results"]["pilot32"]["gate_passed"] is False


def test_recorded_failed_gate_is_blocked_on_resume(tmp_path):
    path = tmp_path / "state.json"
    path.write_text(json.dumps({"stage_results": {
        "prepare": {"status": "complete"},
        "train": {"status": "complete"},
        "seed": {"status": "complete"},
        "pilot32": {"status": "complete", "gate_passed": False},
    }}), encoding="utf-8")

    state = LifecycleController(
        FakeAdapter(), lambda stage, context: pytest.fail(stage), path
    ).run(stop_at="pilot32")
    assert state["status"] == "blocked"
    assert state["phase"] == "pilot32"
    assert state["last_error"] == "stage_gate_not_passed:pilot32"


def test_legacy_gate_delegation_cannot_skip_pilot32(tmp_path):
    calls = []

    def runner(stage, context):
        calls.append(stage)
        if stage == "pilot32":
            return {"status": "skipped", "data": {"gate_delegated": True,
                                                   "gate_basis": "native_legacy_runner"}}
        return {"status": "complete", "gate_passed": True}

    state = LifecycleController(FakeAdapter(), runner, tmp_path / "state.json").run()
    assert calls == ["prepare", "train", "seed", "pilot32"]
    assert state["status"] == "blocked"
    assert state["phase"] == "pilot32"
    assert state["last_error"] == "stage_skipped_not_allowed:pilot32"


def test_legacy_gate_delegation_cannot_skip_validation100(tmp_path):
    calls = []

    def runner(stage, context):
        calls.append(stage)
        if stage == "validation100":
            return {"status": "skipped", "data": {"gate_delegated": True}}
        return {"status": "complete", "gate_passed": True}

    state = LifecycleController(FakeAdapter(), runner, tmp_path / "state.json").run()
    assert calls == ["prepare", "train", "seed", "pilot32", "validation100"]
    assert state["status"] == "blocked"
    assert state["phase"] == "validation100"
    assert state["last_error"] == "stage_skipped_not_allowed:validation100"


@pytest.mark.parametrize("stage", [
    "gepa_prepare", "gepa_run", "comparison500", "publish",
])
def test_gepa_and_publish_stages_cannot_be_skipped(tmp_path, stage):
    """A compatibility marker must never manufacture GEPA/publication evidence."""
    prior = {}
    for name in STAGE_NAMES[:STAGE_NAMES.index(stage)]:
        prior[name] = {"status": "complete"}
        if name in {"pilot32", "validation100", "comparison500"}:
            prior[name]["gate_passed"] = True
    path = tmp_path / f"{stage}.json"
    path.write_text(json.dumps({"stage_results": prior}), encoding="utf-8")

    def runner(current, context):
        assert current == stage
        return {"status": "skipped", "data": {"gate_delegated": True}}

    state = LifecycleController(FakeAdapter(), runner, path).run(start_at=stage)
    assert state["status"] == "blocked"
    assert state["phase"] == stage
    assert state["last_error"] == f"stage_skipped_not_allowed:{stage}"


def test_skipped_gate_receipt_cannot_satisfy_dependency_on_resume(tmp_path):
    """Old pilot receipts with gate_delegated remain blocked on resume."""
    path = tmp_path / "state.json"
    path.write_text(json.dumps({"stage_results": {
        "prepare": {"status": "complete"},
        "train": {"status": "complete"},
        "seed": {"status": "complete"},
        "pilot32": {"status": "skipped", "data": {"gate_delegated": True}},
    }}), encoding="utf-8")
    state = LifecycleController(FakeAdapter(), lambda stage, context: pytest.fail(stage), path).run()
    assert state["status"] == "blocked"
    assert state["phase"] == "pilot32"
    assert state["last_error"] == "stage_skipped_not_allowed:pilot32"


def test_publish_cannot_bypass_failed_comparison_on_resume(tmp_path):
    path = tmp_path / "state.json"
    results = {
        "prepare": {"status": "complete"},
        "train": {"status": "complete"},
        "seed": {"status": "complete"},
        "pilot32": {"status": "complete", "gate_passed": True},
        "validation100": {"status": "complete", "gate_passed": True},
        "gepa_prepare": {"status": "complete"},
        "gepa_run": {"status": "complete"},
        "comparison500": {"status": "complete", "gate_passed": False},
        # A stale receipt from an earlier run must not make this publishable.
        "publish": {"status": "complete"},
    }
    path.write_text(json.dumps({"stage_results": results}))
    state = LifecycleController(FakeAdapter(), lambda stage, context: pytest.fail(stage), path).run()
    assert state["status"] == "blocked"
    assert state["phase"] == "comparison500"
    assert state["last_error"] == "stage_gate_not_passed:comparison500"


def test_callback_failure_keeps_bounded_reason(tmp_path):
    def runner(stage, context):
        if stage == "train":
            raise RuntimeError("remote token=secret-value unavailable")
        return StageResult("complete")

    state = LifecycleController(FakeAdapter(), runner, tmp_path / "state.json").run()
    assert state["status"] == "failed"
    assert state["phase"] == "train"
    assert state["last_error"] == "RuntimeError:remote token=<redacted> unavailable"
    assert state["stage_results"]["train"]["reason"] == state["last_error"]


def test_business_and_appworld_wrappers_use_shared_contract():
    root = Path(__file__).parents[4]
    wrappers = (
        root / "tools/trajectory_evolution/experiments/appworld/reproduce.py",
        root / "tools/trajectory_evolution/experiments/business-coverage/offline_experiment.py",
    )
    for wrapper in wrappers:
        source = wrapper.read_text(encoding="utf-8")
        assert "LifecycleController" in source
        assert "StageResult" in source
        assert "_run_common_" in source
    appworld = wrappers[0].read_text(encoding="utf-8")
    assert "if not isinstance(lifecycle.get('stage_results'), dict)" in appworld
    assert "gate_passed=passed" in appworld
    business = wrappers[1].read_text(encoding="utf-8")
    assert "native_gepa_status:" in business
