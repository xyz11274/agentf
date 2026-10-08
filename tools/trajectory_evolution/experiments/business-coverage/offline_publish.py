#!/usr/bin/env python3
"""离线第三阶段：只发布通过 holdout 门禁的 GEPA L3 版本。"""

from __future__ import annotations

from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

import layout
import offline_gepa as gepa
import run as coverage


def _atomic_write(path: Path, raw: bytes) -> None:
    """用同目录唯一临时文件提交发布登记，并同步文件与目录。"""
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=path.name + ".pending.", dir=path.parent)
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(raw)
            stream.flush()
            os.fsync(stream.fileno())
        temporary.replace(path)
        parent = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(parent)
        finally:
            os.close(parent)
    finally:
        if temporary.exists():
            temporary.unlink()


def _artifact_version(artifact: dict) -> str:
    """调用原生规范化算法重算版本，避免 Python 与 C++ 哈希口径分叉。"""
    result = subprocess.run(
        [str(coverage.BINARY), "artifact-version"],
        input=json.dumps(artifact, ensure_ascii=False, separators=(",", ":")),
        text=True, capture_output=True, timeout=30)
    version = result.stdout.strip()
    if result.returncode or not re.fullmatch(r"agent-version-v1:[0-9a-f]{64}", version):
        raise RuntimeError("gepa_published_artifact_invalid")
    return version


def _publish_locked() -> dict:
    """版本文件只保存一份；latest.json 是指向该版本的轻量登记。"""
    plan = gepa.verify()
    # Publication is downstream of the fixed 500-task paired comparison.  The
    # GEPA holdout gate alone is insufficient because it covers only the
    # search/holdout protocol and cannot prove the historical seed004 scope.
    best_file = layout.PUBLISHED / "best-strategy.json"
    if not best_file.is_file():
        raise RuntimeError("fixed_comparison_missing")
    best = coverage.read(best_file)
    comparison = best.get("comparison") if isinstance(best, dict) else None
    fixed = comparison.get("fixed_500") if isinstance(comparison, dict) else None
    gate = comparison.get("fixed_500_gate") if isinstance(comparison, dict) else None
    if (not isinstance(fixed, dict) or fixed.get("status") != "complete"
            or not isinstance(gate, dict) or gate.get("passed") is not True):
        raise RuntimeError("fixed_comparison_gate_failed")
    status = coverage.read(gepa.STATUS)
    progress = gepa.progress()
    if progress.get("memory_feedback", {}).get("status") not in ("empty", "committed"):
        raise RuntimeError("gepa_memory_feedback_not_settled")
    # 单入口成功后把 published 写入 runner 终态；重复调用只核对并返回同一原子登记。
    if status.get("status") == "published":
        result = status.get("publication")
        latest = layout.PUBLISHED / "latest.json"
        if not isinstance(result, dict) or not latest.is_file() or coverage.read(latest) != result or (
                result.get("source_job") != progress.get("job_id")):
            raise RuntimeError("published_terminal_invalid")
        bundle = layout.PUBLISHED / result.get("bundle", "")
        if not bundle.is_file():
            raise RuntimeError("published_terminal_invalid")
        raw = bundle.read_bytes()
        if hashlib.sha256(raw).hexdigest() != result.get("bundle_sha256"):
            raise RuntimeError("published_terminal_invalid")
        persisted = json.loads(raw)
        if persisted.get("version") != result.get("version") or not isinstance(
                persisted.get("artifact"), dict) or _artifact_version(
                    persisted["artifact"]) != result.get("version"):
            raise RuntimeError("published_terminal_invalid")
        return result
    if status.get("status") != "candidate_ready" or progress.get("job_status") != "passed" or (
            not progress.get("published_in_offline_workspace")):
        raise RuntimeError("gepa_candidate_not_accepted")
    job_id = progress["job_id"]
    source = gepa.WORKSPACE / "agent-evaluation" / job_id / "search/published-version.json"
    if not source.is_file():
        raise RuntimeError("gepa_published_bundle_missing")
    raw = source.read_bytes()
    bundle = json.loads(raw)
    version = bundle.get("version", "")
    match = re.fullmatch(r"agent-version-v1:([0-9a-f]{64})", version)
    if not match or bundle.get("schema_version") != 1 or not isinstance(bundle.get("artifact"), dict) or (
            bundle["artifact"].get("level") != "L3") or (
            bundle["artifact"].get("user_id") != gepa.USER) or (
            bundle["evaluation"].get("job_id") != job_id) or (
            not bundle["evaluation"]["gate"].get("passed")):
        raise RuntimeError("gepa_published_bundle_invalid")
    if _artifact_version(bundle["artifact"]) != version:
        raise RuntimeError("gepa_published_version_mismatch")
    # version 只标识策略 artifact，同一策略可以被不同实验独立复核并再次发布。
    # 证明包还包含 job_id、评分与门禁结果，因此用原始包摘要区分不同证明；
    # 旧证明保持不可变，latest 只切换到本轮已经验证过的证明包。
    bundle_sha256 = hashlib.sha256(raw).hexdigest()
    output = layout.PUBLISHED / "versions" / (
        match.group(1) + "." + bundle_sha256 + ".json")
    output.parent.mkdir(parents=True, exist_ok=True)
    if output.exists():
        if output.read_bytes() != raw:
            raise RuntimeError("published_version_collision")
    else:
        _atomic_write(output, raw)
    result = {
        "schema_version": 1,
        "version": version,
        "bundle": str(output.relative_to(layout.PUBLISHED)),
        "bundle_sha256": bundle_sha256,
        "source_job": job_id,
        "source_dataset_version": plan["dataset_version"],
        "source_memory_hashes": plan["source_memory_hashes"],
        "published_at": datetime.now(timezone.utc).isoformat(),
    }
    _atomic_write(layout.PUBLISHED / "latest.json",
                  (json.dumps(result, ensure_ascii=False, indent=2) + "\n").encode())
    return result


def publish() -> dict:
    """与 fresh 共用生命周期锁；独立调用也不能和清树或另一发布并发。"""
    with gepa.lifecycle_lock():
        return _publish_locked()


if __name__ == "__main__":
    os.umask(0o077)
    print(json.dumps(publish(), ensure_ascii=False, indent=2))
