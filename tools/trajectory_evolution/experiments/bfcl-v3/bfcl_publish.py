#!/usr/bin/env python3
"""Atomic BFCL GEPA publication gate.

The native AgentFlow GEPA job remains the source of candidate and holdout
scores.  This module only verifies its terminal gate and records one compact,
replayable published bundle under trace_output/bfcl-v3/published.
"""
from __future__ import annotations
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]
sys.path.insert(0, str(HERE))
import bfcl_gepa as gepa  # noqa: E402

OUTPUT = gepa.PUBLISHED


def _atomic_write(path: Path, raw: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary_name = tempfile.mkstemp(prefix=path.name + ".pending.", dir=path.parent)
    temporary = Path(temporary_name)
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(raw)
            stream.flush()
            os.fsync(stream.fileno())
        temporary.replace(path)
        directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    finally:
        if temporary.exists():
            temporary.unlink()


def _artifact_version(artifact: dict) -> str:
    result = subprocess.run([str(gepa.BINARY), "artifact-version"],
                            input=json.dumps(artifact, ensure_ascii=False, separators=(",", ":")),
                            text=True, capture_output=True, timeout=30)
    version = result.stdout.strip()
    if result.returncode or not re.fullmatch(r"agent-version-v1:[0-9a-f]{64}", version):
        raise RuntimeError("bfcl_published_artifact_invalid")
    return version


def _load_terminal() -> tuple[dict, dict]:
    plan = gepa.verify()
    status = gepa.read(gepa.STATUS)
    progress = gepa.progress()
    if status.get("status") == "published":
        publication = status.get("publication")
        if not isinstance(publication, dict):
            raise RuntimeError("bfcl_published_terminal_invalid")
        return plan, {"already": publication}
    if status.get("status") != "candidate_ready" or progress.get("job_status") != "passed":
        raise RuntimeError("bfcl_candidate_not_accepted")
    if progress.get("memory_feedback", {}).get("status") not in ("empty", "committed"):
        raise RuntimeError("bfcl_memory_feedback_not_settled")
    job_id = progress.get("job_id", "")
    source = gepa.WORKSPACE / "agent-evaluation" / job_id / "search/published-version.json"
    if not source.is_file():
        raise RuntimeError("bfcl_published_bundle_missing")
    bundle = json.loads(source.read_text(encoding="utf-8"))
    artifact = bundle.get("artifact")
    evaluation = bundle.get("evaluation")
    version = bundle.get("version", "")
    if (bundle.get("schema_version") != 1 or not isinstance(artifact, dict) or
            not isinstance(evaluation, dict) or artifact.get("level") != "L3" or
            artifact.get("user_id") != gepa.USER or evaluation.get("job_id") != job_id or
            not evaluation.get("gate", {}).get("passed")):
        raise RuntimeError("bfcl_published_bundle_invalid")
    if not re.fullmatch(r"agent-version-v1:[0-9a-f]{64}", version) or _artifact_version(artifact) != version:
        raise RuntimeError("bfcl_published_version_mismatch")
    return plan, {"bundle": bundle, "source_job": job_id, "version": version}


def publish() -> dict:
    with gepa.lifecycle_lock():
        plan, terminal = _load_terminal()
        if "already" in terminal:
            return terminal["already"]
        if "bundle" not in terminal or not isinstance(terminal["bundle"], dict):
            raise RuntimeError("bfcl_published_bundle_invalid")
        bundle = terminal["bundle"]
        raw = json.dumps(bundle, ensure_ascii=False, indent=2).encode() + b"\n"
        version = terminal["version"]
        job_id = terminal["source_job"]
        # Existing publication is idempotent and must remain byte-identical.
        bundle_sha256 = hashlib.sha256(raw).hexdigest()
        match = re.fullmatch(r"agent-version-v1:([0-9a-f]{64})", version)
        if not match:
            raise RuntimeError("bfcl_published_version_invalid")
        destination = OUTPUT / "versions" / f"{match.group(1)}.{bundle_sha256}.json"
        if destination.exists() and destination.read_bytes() != raw:
            raise RuntimeError("bfcl_published_version_collision")
        if not destination.exists():
            _atomic_write(destination, raw)
        result = {
            "schema_version": 1, "version": version,
            "bundle": str(destination.relative_to(OUTPUT)),
            "bundle_sha256": bundle_sha256, "source_job": job_id,
            "source_dataset_version": plan.get("dataset_version"),
            "source_memory_hashes": plan.get("source_memory_hashes", {}),
            "published_at": datetime.now(timezone.utc).isoformat(),
        }
        _atomic_write(OUTPUT / "latest.json", (json.dumps(result, ensure_ascii=False, indent=2) + "\n").encode())
        state = gepa.read(gepa.STATUS)
        state.update(status="published", publication=result, published_at=result["published_at"])
        gepa.save(gepa.STATUS, state)
        return result


if __name__ == "__main__":
    os.umask(0o077)
    print(json.dumps(publish(), ensure_ascii=False, indent=2))
