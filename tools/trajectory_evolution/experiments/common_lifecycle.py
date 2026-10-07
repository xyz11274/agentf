#!/usr/bin/env python3
"""Shared offline lifecycle contract for trajectory-evolution experiments.

This module is the shared lifecycle contract used by the business and
AppWorld wrappers.  Native runners remain the owners of checkpoints and
metrics; this controller supplies only the common stage order, durable state
shape, dependency/gate checks, and dataset adapter hooks.

The model-facing work is supplied by ``run_stage``.  This module never starts
an AgentFlow process, calls a model, computes a score, or rewrites a trial.
That keeps the contract safe to exercise with a fake runner and prevents a
future adapter from silently introducing a second evaluator.
"""
from __future__ import annotations

from dataclasses import dataclass, field
import json
import os
from pathlib import Path
import re
import time
from typing import Any, Mapping, Protocol


SCHEMA_VERSION = 1


class LifecycleError(RuntimeError):
    """A lifecycle contract or dependency violation."""


class DatasetAdapter(Protocol):
    """Dataset-specific hooks; no algorithm or scoring logic belongs here.

    An adapter supplies identity and task materialization only.  ``split_counts``
    and ``materialize`` must describe the canonical source dataset; they must
    not choose tasks from scores or mutate an existing execution report.
    ``baseline`` returns an opaque native artifact/report reference for a
    requested scope.  The common controller passes these values to the stage
    runner without interpreting them.
    """

    @property
    def name(self) -> str: ...

    @property
    def output_root(self) -> Path: ...

    def split_counts(self) -> Mapping[str, int]: ...

    def materialize(self, view: str) -> Mapping[str, Any]: ...

    def baseline(self, scope: str) -> Mapping[str, Any]: ...


@dataclass(frozen=True)
class StageSpec:
    """One shared stage and the evidence it must have before starting."""

    name: str
    requires: tuple[str, ...] = ()
    gate_requires: tuple[str, ...] = ()


# Keep this list as the single ordering contract.  Dataset adapters can change
# task IDs and host commands, but cannot reorder training, seed, gates, GEPA,
# final comparison, or publication.
STAGES: tuple[StageSpec, ...] = (
    StageSpec("prepare"),
    StageSpec("train", ("prepare",)),
    StageSpec("seed", ("train",)),
    StageSpec("pilot32", ("seed",)),
    StageSpec("validation100", ("pilot32",), ("pilot32",)),
    StageSpec("gepa_prepare", ("validation100",), ("validation100",)),
    StageSpec("gepa_run", ("gepa_prepare",)),
    StageSpec("comparison500", ("gepa_run",)),
    # Publication is a gated transition.  A complete comparison artifact is
    # useful evidence, but it is never enough to publish when its native gate
    # explicitly failed.
    StageSpec("publish", ("comparison500",), ("comparison500",)),
)

STAGE_NAMES = tuple(spec.name for spec in STAGES)
# ``skipped`` remains a narrow compatibility status for preparatory stages
# whose native runner may fold the work into a later atomic operation.  It is
# deliberately *not* accepted for pilot/validation gates, GEPA, comparison,
# or publication: those stages are the evidence boundary of the shared flow.
# In particular, a ``gate_delegated`` marker is descriptive metadata only; it
# cannot turn an absent stage result into a pass.
TERMINAL_STAGE_STATUSES = frozenset({"complete", "skipped", "failed", "blocked", "pending"})
# A skipped stage can only satisfy a dependency when it is a preparatory
# compatibility step.  Every gated/evaluation/publication stage must execute
# and return its own evidence.  Keep this explicit so new stages do not
# accidentally inherit a legacy bypass.
SKIPPABLE_STAGES = frozenset({"prepare", "train", "seed"})


@dataclass(frozen=True)
class StageResult:
    """Canonical wrapper around native stage evidence.

    ``metrics`` and ``gate_passed`` are copied only when the adapter/native
    runner supplies them.  Missing values remain missing; this contract never
    treats an absent metric as zero or infers a pass from partial rows.
    """

    status: str
    data: Mapping[str, Any] = field(default_factory=dict)
    metrics: Mapping[str, Any] | None = None
    gate_passed: bool | None = None
    artifact: str | None = None
    reason: str | None = None

    def as_dict(self) -> dict[str, Any]:
        result: dict[str, Any] = {
            "status": self.status,
            "data": dict(self.data),
        }
        if self.metrics is not None:
            result["metrics"] = dict(self.metrics)
        if self.gate_passed is not None:
            result["gate_passed"] = self.gate_passed
        if self.artifact is not None:
            result["artifact"] = self.artifact
        if self.reason is not None:
            result["reason"] = self.reason
        return result


class StageRunner(Protocol):
    """Adapter-owned call into the existing native stage implementation."""

    def __call__(self, stage: str, context: Mapping[str, Any]) -> StageResult | Mapping[str, Any]: ...


def _json_object(value: Any, label: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise LifecycleError(f"{label}_must_be_object")
    return value


def _normalize_result(value: StageResult | Mapping[str, Any], stage: str) -> StageResult:
    if isinstance(value, StageResult):
        result = value
    else:
        raw = _json_object(dict(value), f"{stage}_result")
        status = raw.get("status", "complete")
        result = StageResult(
            status=status,
            data=_json_object(raw.get("data", {}), f"{stage}_data"),
            metrics=(None if raw.get("metrics") is None else
                     _json_object(raw["metrics"], f"{stage}_metrics")),
            gate_passed=raw.get("gate_passed"),
            artifact=raw.get("artifact"),
            reason=raw.get("reason"),
        )
    if result.status not in TERMINAL_STAGE_STATUSES:
        raise LifecycleError(f"{stage}_status_invalid")
    if result.gate_passed is not None and type(result.gate_passed) is not bool:
        raise LifecycleError(f"{stage}_gate_invalid")
    if result.artifact is not None and not isinstance(result.artifact, str):
        raise LifecycleError(f"{stage}_artifact_invalid")
    if result.status == "skipped" and stage not in SKIPPABLE_STAGES:
        raise LifecycleError(f"stage_skipped_not_allowed:{stage}")
    return result


def _failure_reason(exc: Exception) -> str:
    """Keep a bounded, redacted native reason in the compact receipt.

    The controller must not retain a traceback or prompt/credential payload,
    but a type-only marker made recovery diagnosis needlessly opaque.
    """
    message = str(exc).strip()
    message = re.sub(
        r"(?i)(api[_-]?key|authorization|password|token)\s*[:=]\s*[^\s,;]+",
        r"\1=<redacted>", message,
    )
    if len(message) > 240:
        message = message[:237] + "..."
    return f"{type(exc).__name__}:{message}" if message else type(exc).__name__


class StateStore:
    """Tiny atomic JSON store used by the controller's lifecycle state only."""

    def __init__(self, path: Path):
        self.path = Path(path)

    def load(self) -> dict[str, Any]:
        if not self.path.is_file():
            return {
                "schema_version": SCHEMA_VERSION,
                "status": "pending",
                "phase": "pending",
                "stage_results": {},
                "artifacts": {},
                "metrics": {},
            }
        try:
            value = json.loads(self.path.read_text(encoding="utf-8"))
        except (OSError, ValueError) as exc:
            raise LifecycleError("lifecycle_state_invalid") from exc
        value = _json_object(value, "lifecycle_state")
        # Existing runners predate this contract and may have no schema
        # marker.  Preserve their fields and attach the common marker on the
        # next atomic write; a different explicit schema remains a hard error.
        if value.get("schema_version") not in (None, SCHEMA_VERSION):
            raise LifecycleError("lifecycle_state_schema_mismatch")
        value.setdefault("schema_version", SCHEMA_VERSION)
        if not isinstance(value.get("stage_results", {}), dict):
            raise LifecycleError("lifecycle_stage_results_invalid")
        return value

    def save(self, value: Mapping[str, Any]) -> None:
        payload = dict(value)
        payload["schema_version"] = SCHEMA_VERSION
        payload["updated_at_ns"] = time.time_ns()
        self.path.parent.mkdir(parents=True, exist_ok=True)
        temporary = self.path.with_name(self.path.name + ".new")
        temporary.write_text(
            json.dumps(payload, ensure_ascii=False, separators=(",", ":")) + "\n",
            encoding="utf-8",
        )
        os.replace(temporary, self.path)


class LifecycleController:
    """Run the common stage contract around an existing native implementation.

    ``resume`` is intentionally unconditional with respect to mutable code
    fingerprints.  It reuses only completed stage records in the state file;
    the adapter/native stage remains responsible for validating task scope and
    preserving authoritative reports.  This avoids the old version-check
    deadlocks while keeping the score and execution evidence immutable.
    """

    def __init__(self, adapter: DatasetAdapter, runner: StageRunner, state_path: Path):
        self.adapter = adapter
        self.runner = runner
        self.store = StateStore(Path(state_path))

    def _spec(self, stage: str) -> StageSpec:
        for spec in STAGES:
            if spec.name == stage:
                return spec
        raise LifecycleError(f"unknown_stage:{stage}")

    @staticmethod
    def _stage_complete(state: Mapping[str, Any], stage: str) -> bool:
        value = (state.get("stage_results") or {}).get(stage)
        if not isinstance(value, dict):
            return False
        status = value.get("status")
        return status == "complete" or (status == "skipped" and stage in SKIPPABLE_STAGES)

    def _check_requirements(self, state: Mapping[str, Any], spec: StageSpec) -> None:
        for dependency in spec.requires:
            if not self._stage_complete(state, dependency):
                raise LifecycleError(f"stage_dependency_incomplete:{spec.name}:{dependency}")
        for dependency in spec.gate_requires:
            result = (state.get("stage_results") or {}).get(dependency) or {}
            # ``gate_delegated`` is retained as diagnostic metadata for old
            # receipts, but never waives a common gate.  A gate is passed only
            # by an executed stage that records ``gate_passed: true``.
            if result.get("status") == "skipped":
                raise LifecycleError(f"stage_gate_skipped:{spec.name}:{dependency}")
            if result.get("gate_passed") is not True:
                raise LifecycleError(f"stage_gate_not_passed:{spec.name}:{dependency}")

    def _context(self, state: Mapping[str, Any], spec: StageSpec) -> dict[str, Any]:
        """Build adapter context without copying traces or credentials."""
        return {
            "stage": spec.name,
            "dataset": {
                "name": self.adapter.name,
                "split_counts": dict(self.adapter.split_counts()),
                "output_root": str(self.adapter.output_root),
            },
            "state": state,
        }

    def run(self, *, start_at: str | None = None,
            stop_at: str | None = None) -> dict[str, Any]:
        """Run one contiguous prefix of the shared flow.

        ``start_at`` is retained for recovery fixtures; normal public entry
        points should use ``stop_at`` so dependencies are always replayed or
        reused through this controller.  A stopped run records completed
        evidence but does not mark the experiment published until the final
        ``publish`` stage is reached.
        """
        state = self.store.load()
        state.setdefault("dataset", {
            "name": self.adapter.name,
            "split_counts": dict(self.adapter.split_counts()),
            "output_root": str(self.adapter.output_root),
        })
        state["status"] = "running"
        if start_at is not None and start_at not in STAGE_NAMES:
            raise LifecycleError(f"unknown_start_stage:{start_at}")
        if stop_at is not None and stop_at not in STAGE_NAMES:
            raise LifecycleError(f"unknown_stop_stage:{stop_at}")
        start_index = STAGE_NAMES.index(start_at) if start_at is not None else 0
        stop_index = STAGE_NAMES.index(stop_at) if stop_at is not None else len(STAGES) - 1
        if stop_index < start_index:
            raise LifecycleError("stage_stop_before_start")
        self.store.save(state)
        for index, spec in enumerate(STAGES):
            if index < start_index:
                continue
            recorded = (state.get("stage_results") or {}).get(spec.name)
            # Do not re-enter a stage whose old receipt claimed ``skipped``
            # outside the narrow compatibility allowlist.  Otherwise a resume
            # could call an adapter again and accidentally reinterpret a legacy
            # gate waiver as fresh evidence.
            if (isinstance(recorded, dict) and recorded.get("status") == "skipped"
                    and spec.name not in SKIPPABLE_STAGES):
                state["status"] = "blocked"
                state["phase"] = spec.name
                state["last_error"] = f"stage_skipped_not_allowed:{spec.name}"
                self.store.save(state)
                return state
            if self._stage_complete(state, spec.name):
                # A previously recorded terminal stage is reusable only while
                # its own dependencies and gates still hold.  In particular,
                # a stale ``publish=complete`` must not bypass a newly
                # invalidated comparison500 gate on resume.
                if isinstance(recorded, dict) and recorded.get("gate_passed") is False:
                    state["status"] = "blocked"
                    state["phase"] = spec.name
                    state["last_error"] = f"stage_gate_not_passed:{spec.name}"
                    self.store.save(state)
                    return state
                try:
                    self._check_requirements(state, spec)
                except LifecycleError as exc:
                    state["status"] = "blocked"
                    state["phase"] = spec.name
                    state["last_error"] = str(exc)
                    self.store.save(state)
                    return state
                if index == stop_index:
                    state["status"] = "published" if spec.name == "publish" else "complete"
                    state["phase"] = spec.name
                    self.store.save(state)
                    return state
                continue
            try:
                self._check_requirements(state, spec)
                state["phase"] = spec.name
                self.store.save(state)
                result = _normalize_result(self.runner(spec.name, self._context(state, spec)), spec.name)
            except LifecycleError as exc:
                state["status"] = "blocked"
                state["phase"] = spec.name
                state["last_error"] = str(exc)
                self.store.save(state)
                return state
            except Exception as exc:  # pragma: no cover - adapter-owned failure boundary
                result = StageResult("failed", reason=_failure_reason(exc))
            state.setdefault("stage_results", {})[spec.name] = result.as_dict()
            if result.artifact is not None:
                state.setdefault("artifacts", {})[spec.name] = result.artifact
            if result.metrics is not None:
                state.setdefault("metrics", {})[spec.name] = dict(result.metrics)
            self.store.save(state)
            if result.gate_passed is False:
                state["status"] = "blocked"
                state["phase"] = spec.name
                state["last_error"] = f"stage_gate_not_passed:{spec.name}"
                self.store.save(state)
                return state
            if result.status not in {"complete", "skipped"}:
                state["status"] = result.status
                state["last_error"] = result.reason or f"{spec.name}_failed"
                self.store.save(state)
                return state
            if index == stop_index:
                state["status"] = "published" if spec.name == "publish" else "complete"
                state["phase"] = spec.name
                self.store.save(state)
                return state
        state["status"] = "published" if self._stage_complete(state, "publish") else "complete"
        state["phase"] = "publish" if state["status"] == "published" else state.get("phase", "pending")
        self.store.save(state)
        return state


__all__ = [
    "DatasetAdapter",
    "LifecycleController",
    "LifecycleError",
    "SCHEMA_VERSION",
    "SKIPPABLE_STAGES",
    "STAGES",
    "STAGE_NAMES",
    "StageResult",
    "StageRunner",
    "StateStore",
]
