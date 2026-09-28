#!/usr/bin/env python3
"""Offline, lossless archive for trajectory-evolution evaluation artifacts.

The evaluator historically wrote the same trial three times: embedded in
``report.json``, in ``results.jsonl`` and in ``trials/<id>.json``.  This tool
keeps the original tree untouched and writes a content-addressed, zstd-compressed
archive.  When the byte-for-byte relationship can be proved, the latter two
representations are recorded as derived entries and are regenerated on restore.

The archive is deliberately independent of the C++ reader.  It is therefore a
safe offline operation for old runs while preserving the original report and
manifest bytes and their SHA-256 values.

Examples::

    python artifact_store.py archive \
      --source /path/to/agent-evaluation \
      --output /path/to/agent-evaluation.archive
    python artifact_store.py verify --archive /path/to/agent-evaluation.archive
    python artifact_store.py restore --archive /path/to/agent-evaluation.archive \
      --output /tmp/agent-evaluation-restored

No source file is removed or modified by any command.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
import shutil
import sys
import tempfile
from pathlib import Path, PurePosixPath
from typing import Any, Iterable

CHUNK = 1024 * 1024
SCHEMA = "agentflow-artifact-archive-v1"


class ArchiveError(RuntimeError):
    """Raised for a corrupt source or archive."""


def _zstd_module():
    try:
        import zstandard  # type: ignore
    except ImportError as exc:  # pragma: no cover - exercised on minimal hosts
        raise ArchiveError(
            "zstandard Python module is required (install zstandard or use the "
            "project's runtime environment)"
        ) from exc
    return zstandard


def _digest(path: Path) -> tuple[str, int]:
    h = hashlib.sha256()
    size = 0
    with path.open("rb") as handle:
        while True:
            chunk = handle.read(CHUNK)
            if not chunk:
                break
            h.update(chunk)
            size += len(chunk)
    return h.hexdigest(), size


def _digest_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _json_bytes(value: Any) -> bytes:
    # This is the serialization used by task_evaluation for individual trial
    # JSON files.  We only use it after comparing the produced bytes to source.
    return json.dumps(
        value, ensure_ascii=False, separators=(",", ":"), allow_nan=False
    ).encode("utf-8")


def _safe_relative(path: Path, root: Path) -> str:
    try:
        rel = path.resolve().relative_to(root.resolve())
    except ValueError as exc:
        raise ArchiveError(f"path escapes source root: {path}") from exc
    if path.is_symlink():
        raise ArchiveError(f"symlinks are not supported: {path}")
    return PurePosixPath(rel.as_posix()).as_posix()


def _safe_output_path(root: Path, relative: str) -> Path:
    p = PurePosixPath(relative)
    if p.is_absolute() or ".." in p.parts or not relative:
        raise ArchiveError(f"unsafe archive path: {relative!r}")
    result = (root / Path(*p.parts)).resolve()
    try:
        result.relative_to(root.resolve())
    except ValueError as exc:
        raise ArchiveError(f"archive path escapes output: {relative!r}") from exc
    return result


def _iter_source_files(root: Path) -> list[Path]:
    files: list[Path] = []
    for path in root.rglob("*"):
        if path.is_symlink():
            raise ArchiveError(f"source contains unsupported symlink: {path}")
        if path.is_file():
            files.append(path)
    return sorted(files, key=lambda p: _safe_relative(p, root))


def _manifest_preflight(root: Path) -> dict[str, int]:
    """Validate every manifest entry before any source is archived."""
    manifests = sorted(root.rglob("manifest.json"))
    checked_files = 0
    checked_reports = 0
    for manifest_path in manifests:
        try:
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        except Exception as exc:
            raise ArchiveError(f"invalid manifest JSON: {manifest_path}: {exc}") from exc
        files = manifest.get("files")
        if not isinstance(files, dict):
            raise ArchiveError(f"manifest files is not an object: {manifest_path}")
        for name, expected in files.items():
            if not isinstance(name, str) or not isinstance(expected, str):
                raise ArchiveError(f"invalid manifest file entry: {manifest_path}: {name!r}")
            candidate = (manifest_path.parent / Path(name)).resolve()
            try:
                candidate.relative_to(root.resolve())
            except ValueError as exc:
                raise ArchiveError(f"manifest path escapes source: {manifest_path}: {name}") from exc
            if not candidate.is_file():
                raise ArchiveError(f"manifest references missing file: {manifest_path}: {name}")
            actual, _ = _digest(candidate)
            if actual != expected:
                raise ArchiveError(
                    f"manifest SHA-256 mismatch: {manifest_path}: {name} "
                    f"expected={expected} actual={actual}"
                )
            checked_files += 1
        report = manifest_path.parent / "report.json"
        if report.is_file():
            try:
                parsed = json.loads(report.read_text(encoding="utf-8"))
            except Exception as exc:
                raise ArchiveError(f"invalid report JSON: {report}: {exc}") from exc
            _validate_report(parsed, report)
            checked_reports += 1
    return {"manifests": len(manifests), "manifest_files": checked_files, "reports": checked_reports}


def _validate_report(report: Any, path: Path) -> None:
    if not isinstance(report, dict):
        raise ArchiveError(f"report is not an object: {path}")
    trials = report.get("trials")
    if trials is None:
        return
    if not isinstance(trials, list):
        raise ArchiveError(f"report trials is not an array: {path}")
    ids: set[str] = set()
    for index, trial in enumerate(trials):
        if not isinstance(trial, dict):
            raise ArchiveError(f"report trial {index} is not an object: {path}")
        trial_id = trial.get("trial_id")
        if trial_id is None:
            continue
        if not isinstance(trial_id, str) or trial_id in ids:
            raise ArchiveError(f"duplicate/invalid trial_id in {path}: {trial_id!r}")
        ids.add(trial_id)


def _load_derivation(report_path: Path) -> dict[str, Any] | None:
    """Return exact trial JSON bytes that can be regenerated from report.json."""
    try:
        parsed = json.loads(report_path.read_text(encoding="utf-8"))
    except Exception:
        return None
    trials = parsed.get("trials") if isinstance(parsed, dict) else None
    if not isinstance(trials, list):
        return None
    by_id: dict[str, bytes] = {}
    for trial in trials:
        if not isinstance(trial, dict) or not isinstance(trial.get("trial_id"), str):
            return None
        try:
            by_id[trial["trial_id"]] = _json_bytes(trial)
        except (TypeError, ValueError):
            return None
    return {"path": report_path, "trials": by_id}


def _ending_hex(raw: bytes, body: bytes) -> str | None:
    if not raw.startswith(body):
        return None
    ending = raw[len(body) :]
    if ending not in (b"", b"\n", b"\r\n"):
        return None
    return ending.hex()


def _derive_entries(root: Path, files: list[Path]) -> dict[str, dict[str, Any]]:
    """Find only proven report→trial/results byte relationships."""
    result: dict[str, dict[str, Any]] = {}
    # Do not retain parsed reports for the entire tree.  A single large report
    # can expand to several GiB as Python objects; retaining all of them made a
    # full GEPA archive unnecessarily memory hungry.  Process one attempt and
    # release its trial map before moving to the next one.
    report_dirs = sorted({path.parent for path in files if path.name == "report.json"})
    for directory in report_dirs:
        derivation = _load_derivation(directory / "report.json")
        if not derivation:
            continue
        report_rel = _safe_relative(derivation["path"], root)
        trial_dir = directory / "trials"
        if trial_dir.is_dir():
            for trial_path in sorted(trial_dir.glob("*.json")):
                raw = trial_path.read_bytes()
                trial_id = trial_path.stem
                body = derivation["trials"].get(trial_id)
                ending = _ending_hex(raw, body) if body is not None else None
                if ending is None:
                    continue
                digest = _digest_bytes(raw)
                result[_safe_relative(trial_path, root)] = {
                    "sha256": digest,
                    "size": len(raw),
                    "kind": "trial",
                    "from": report_rel,
                    "trial_id": trial_id,
                    "ending_hex": ending,
                }
        lines_path = directory / "results.jsonl"
        if lines_path.is_file():
            raw = lines_path.read_bytes()
            lines = raw.splitlines(keepends=True)
            line_ids: list[str] = []
            endings: list[str] = []
            valid = bool(lines)
            for line in lines:
                body = line.rstrip(b"\r\n")
                try:
                    item = json.loads(body)
                    trial_id = item.get("trial_id") if isinstance(item, dict) else None
                except Exception:
                    trial_id = None
                expected = derivation["trials"].get(trial_id) if isinstance(trial_id, str) else None
                ending = _ending_hex(line, expected) if expected is not None else None
                if ending is None:
                    valid = False
                    break
                line_ids.append(trial_id)
                endings.append(ending)
            if valid:
                result[_safe_relative(lines_path, root)] = {
                    "sha256": _digest_bytes(raw),
                    "size": len(raw),
                    "kind": "results_jsonl",
                    "from": report_rel,
                    "trial_ids": line_ids,
                    "endings_hex": endings,
                }
        del derivation
    return result


def _compress(source: Path, destination: Path, level: int) -> None:
    zstd = _zstd_module()
    destination.parent.mkdir(parents=True, exist_ok=True)
    compressor = zstd.ZstdCompressor(level=level)
    with source.open("rb") as inp, destination.open("wb") as out:
        with compressor.stream_writer(out) as writer:
            while True:
                chunk = inp.read(CHUNK)
                if not chunk:
                    break
                writer.write(chunk)


def _decompress(source: Path, destination: Path) -> None:
    zstd = _zstd_module()
    destination.parent.mkdir(parents=True, exist_ok=True)
    decompressor = zstd.ZstdDecompressor()
    with source.open("rb") as inp, decompressor.stream_reader(inp) as reader, destination.open("wb") as out:
        while True:
            chunk = reader.read(CHUNK)
            if not chunk:
                break
            out.write(chunk)


def _archive(source: Path, output: Path, level: int) -> dict[str, Any]:
    source = source.resolve()
    output = output.resolve()
    if not source.is_dir():
        raise ArchiveError(f"source is not a directory: {source}")
    if output == source or source in output.parents:
        raise ArchiveError("archive output must be outside source tree")
    if output.exists():
        raise ArchiveError(f"archive output already exists: {output} (use a new path)")
    output.parent.mkdir(parents=True, exist_ok=True)
    preflight = _manifest_preflight(source)
    source_files = _iter_source_files(source)
    derived = _derive_entries(source, source_files)
    temp = Path(tempfile.mkdtemp(prefix=f".{output.name}.tmp-", dir=str(output.parent)))
    try:
        blobs = temp / "blobs"
        records: dict[str, dict[str, Any]] = {}
        blob_sizes: dict[str, int] = {}
        source_bytes = 0
        stored_bytes = 0
        derived_bytes = 0
        for path in source_files:
            rel = _safe_relative(path, source)
            digest, size = _digest(path)
            source_bytes += size
            if rel in derived:
                entry = dict(derived[rel])
                if entry["sha256"] != digest or entry["size"] != size:
                    raise ArchiveError(f"derived entry changed during archive: {rel}")
                records[rel] = {"derived": entry}
                derived_bytes += size
                continue
            blob = blobs / f"{digest}.zst"
            if digest not in blob_sizes:
                _compress(path, blob, level)
                blob_sizes[digest] = blob.stat().st_size
                stored_bytes += blob_sizes[digest]
            records[rel] = {"sha256": digest, "size": size, "blob": f"blobs/{digest}.zst"}
        metadata: dict[str, Any] = {
            "schema": SCHEMA,
            "source_name": source.name,
            "compression": {"codec": "zstd", "level": level},
            "verification": preflight,
            "files": records,
            "stats": {
                "source_bytes": source_bytes,
                "stored_blob_bytes": stored_bytes,
                "deduplicated_files": len(source_files) - len(blob_sizes) - len(derived),
                "derived_files": len(derived),
                "source_files": len(source_files),
            },
        }
        (temp / "archive.json").write_text(
            json.dumps(metadata, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        temp.rename(output)
        metadata["stats"]["archive_bytes"] = sum(p.stat().st_size for p in output.rglob("*") if p.is_file())
        return metadata
    except Exception:
        shutil.rmtree(temp, ignore_errors=True)
        raise


def _read_archive(archive: Path) -> tuple[Path, dict[str, Any]]:
    archive = archive.resolve()
    if not archive.is_dir():
        raise ArchiveError(f"archive is not a directory: {archive}")
    try:
        metadata = json.loads((archive / "archive.json").read_text(encoding="utf-8"))
    except Exception as exc:
        raise ArchiveError(f"invalid archive index: {archive}: {exc}") from exc
    if metadata.get("schema") != SCHEMA or not isinstance(metadata.get("files"), dict):
        raise ArchiveError(f"unsupported archive schema: {archive}")
    return archive, metadata


def _record_bytes(archive: Path, relative: str, records: dict[str, Any], cache: dict[str, bytes]) -> bytes:
    record = records.get(relative)
    if not isinstance(record, dict) or "blob" not in record:
        raise ArchiveError(f"derived source is not a stored file: {relative}")
    blob = archive / record["blob"]
    if not blob.is_file():
        raise ArchiveError(f"missing archive blob: {blob}")
    digest = record["sha256"]
    if digest not in cache:
        zstd = _zstd_module()
        # stream_writer() deliberately omits a content-size field so that
        # large evidence files never need their size in memory up front.
        # Use stream_reader rather than ZstdDecompressor.decompress(), which
        # requires that optional field.
        output = io.BytesIO()
        try:
            with blob.open("rb") as inp, zstd.ZstdDecompressor().stream_reader(inp) as reader:
                while True:
                    chunk = reader.read(CHUNK)
                    if not chunk:
                        break
                    output.write(chunk)
        except Exception as exc:
            raise ArchiveError(f"cannot decompress archive blob: {blob}: {exc}") from exc
        cache[digest] = output.getvalue()
    data = cache[digest]
    if len(data) != record["size"] or _digest_bytes(data) != digest:
        raise ArchiveError(f"archive blob verification failed: {relative}")
    return data


def _derived_bytes(archive: Path, entry: dict[str, Any], records: dict[str, Any], cache: dict[str, bytes]) -> bytes:
    report_path = entry.get("from")
    report_raw = _record_bytes(archive, report_path, records, cache)
    try:
        report = json.loads(report_raw)
    except Exception as exc:
        raise ArchiveError(f"cannot parse derived report {report_path}: {exc}") from exc
    by_id = {
        item.get("trial_id"): _json_bytes(item)
        for item in report.get("trials", [])
        if isinstance(item, dict) and isinstance(item.get("trial_id"), str)
    }
    if entry.get("kind") == "trial":
        body = by_id.get(entry.get("trial_id"))
        if body is None:
            raise ArchiveError(f"missing derived trial {entry.get('trial_id')}")
        return body + bytes.fromhex(entry.get("ending_hex", ""))
    if entry.get("kind") == "results_jsonl":
        out = bytearray()
        for trial_id, ending in zip(entry.get("trial_ids", []), entry.get("endings_hex", [])):
            body = by_id.get(trial_id)
            if body is None:
                raise ArchiveError(f"missing derived result trial {trial_id}")
            out.extend(body)
            out.extend(bytes.fromhex(ending))
        return bytes(out)
    raise ArchiveError(f"unsupported derived kind: {entry.get('kind')}")


def _derived_bytes_from_trial_map(entry: dict[str, Any], by_id: dict[str, bytes]) -> bytes:
    """Rebuild one derived file from a parsed report trial map."""
    if entry.get("kind") == "trial":
        body = by_id.get(entry.get("trial_id"))
        if body is None:
            raise ArchiveError(f"missing derived trial {entry.get('trial_id')}")
        return body + bytes.fromhex(entry.get("ending_hex", ""))
    if entry.get("kind") == "results_jsonl":
        out = bytearray()
        for trial_id, ending in zip(entry.get("trial_ids", []), entry.get("endings_hex", [])):
            body = by_id.get(trial_id)
            if body is None:
                raise ArchiveError(f"missing derived result trial {trial_id}")
            out.extend(body)
            out.extend(bytes.fromhex(ending))
        return bytes(out)
    raise ArchiveError(f"unsupported derived kind: {entry.get('kind')}")


def _verify(archive: Path, deep: bool = True) -> dict[str, int]:
    archive, metadata = _read_archive(archive)
    records = metadata["files"]
    blobs_checked: set[str] = set()
    bytes_checked = 0
    derived_by_report: dict[str, list[tuple[str, dict[str, Any]]]] = {}
    for relative, record in records.items():
        if not isinstance(record, dict):
            raise ArchiveError(f"invalid archive record: {relative}")
        if "derived" in record:
            if deep:
                report_path = record["derived"].get("from")
                if not isinstance(report_path, str):
                    raise ArchiveError(f"derived record has no report source: {relative}")
                derived_by_report.setdefault(report_path, []).append((relative, record["derived"]))
            continue
        blob = archive / record.get("blob", "")
        if not blob.is_file():
            raise ArchiveError(f"missing archive blob for {relative}: {blob}")
        key = record.get("sha256")
        if not isinstance(key, str):
            raise ArchiveError(f"invalid digest for {relative}")
        if key not in blobs_checked:
            # Unique blobs are checked once; no cross-record cache is needed.
            data = _record_bytes(archive, relative, records, {})
            blobs_checked.add(key)
            bytes_checked += len(data)
    if deep:
        # A report can be hundreds of megabytes and may have thousands of
        # derived trial files.  Process one report group at a time so it is
        # decompressed and parsed once, without retaining the whole archive.
        for report_path, entries in derived_by_report.items():
            report_raw = _record_bytes(archive, report_path, records, {})
            try:
                report = json.loads(report_raw)
            except Exception as exc:
                raise ArchiveError(f"cannot parse derived report {report_path}: {exc}") from exc
            by_id = {
                item.get("trial_id"): _json_bytes(item)
                for item in report.get("trials", [])
                if isinstance(item, dict) and isinstance(item.get("trial_id"), str)
            }
            for relative, expected in entries:
                data = _derived_bytes_from_trial_map(expected, by_id)
                if len(data) != expected["size"] or _digest_bytes(data) != expected["sha256"]:
                    raise ArchiveError(f"derived record verification failed: {relative}")
                bytes_checked += len(data)
            del by_id, report, report_raw
    return {"files": len(records), "blobs": len(blobs_checked), "bytes_checked": bytes_checked}


def _restore(archive: Path, output: Path, force: bool) -> dict[str, int]:
    archive, metadata = _read_archive(archive)
    output = output.resolve()
    if output.exists():
        if not force:
            raise ArchiveError(f"restore output exists: {output} (use --force)")
        shutil.rmtree(output)
    output.mkdir(parents=True)
    records = metadata["files"]
    cache: dict[str, bytes] = {}
    restored = 0
    for relative, record in records.items():
        destination = _safe_output_path(output, relative)
        destination.parent.mkdir(parents=True, exist_ok=True)
        if "derived" in record:
            data = _derived_bytes(archive, record["derived"], records, cache)
            expected = record["derived"]
        else:
            data = _record_bytes(archive, relative, records, cache)
            expected = record
        if len(data) != expected["size"] or _digest_bytes(data) != expected["sha256"]:
            raise ArchiveError(f"restore verification failed: {relative}")
        destination.write_bytes(data)
        restored += 1
    return {"files": restored}


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    archive = commands.add_parser("archive", help="verify and compress a source tree")
    archive.add_argument("--source", type=Path, required=True)
    archive.add_argument("--output", type=Path, required=True)
    archive.add_argument("--zstd-level", type=int, default=3)
    verify = commands.add_parser("verify", help="verify archive blobs and derived entries")
    verify.add_argument("--archive", type=Path, required=True)
    verify.add_argument("--metadata-only", action="store_true", help="skip decompression/digest pass")
    restore = commands.add_parser("restore", help="restore a complete source tree")
    restore.add_argument("--archive", type=Path, required=True)
    restore.add_argument("--output", type=Path, required=True)
    restore.add_argument("--force", action="store_true")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        if args.command == "archive":
            if not 1 <= args.zstd_level <= 22:
                raise ArchiveError("--zstd-level must be between 1 and 22")
            metadata = _archive(args.source, args.output, args.zstd_level)
            print(json.dumps({"status": "archived", **metadata["stats"], "verification": metadata["verification"]}, ensure_ascii=False, sort_keys=True))
        elif args.command == "verify":
            result = _verify(args.archive, deep=not args.metadata_only)
            print(json.dumps({"status": "verified", **result}, ensure_ascii=False, sort_keys=True))
        else:
            result = _restore(args.archive, args.output, args.force)
            print(json.dumps({"status": "restored", **result}, ensure_ascii=False, sort_keys=True))
        return 0
    except (ArchiveError, OSError, ValueError) as exc:
        print(f"artifact_store: error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
