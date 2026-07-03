"""AgentFlow Bridge — Agent Package HTTP routes.

This router owns every endpoint mounted under ``/api/packages`` and is split
into two layers:

* **T5 file-system CRUD layer** (list / tree / read / write / create / delete
  / rename + directory operations) — pure filesystem operations over the
  on-disk package trees under ``settings.agent_packages_path``. The
  filesystem is the single source of truth; nothing is cached.

* **T8 packaging layer** (POST/GET/GET-download ``/package``) — wraps
  ``publish/agent_build.sh`` as a subprocess, computes a sha256 of the
  produced tarball, and streams the artefact back to the caller.

All file operations go through :func:`_validate_relative_path` /
:func:`_resolve_within` which reject ``..`` path traversal before any FS
access happens (I-2 boundary hard constraint).
"""

from __future__ import annotations

import asyncio
import hashlib
import logging
import shutil
import stat
import tarfile
import tempfile
import zipfile
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, List

from fastapi import APIRouter, Depends, File, Form, HTTPException, UploadFile, status
from fastapi import Path as PathParam
from fastapi.responses import FileResponse, JSONResponse
from pydantic import BaseModel, Field

from client import DaemonClient, DaemonError
from config import settings
from dependencies import get_daemon_client
from models import FileNode, PackageInfo

logger = logging.getLogger("agentflow.bridge.routes.packages")

router = APIRouter(prefix="/api/packages", tags=["packages"])

# --- constants --------------------------------------------------------------

# Subprocess timeout for the build script. 60s is generous for tar+gzip of
# any reasonable agent package and short enough that a hung build won't
# pin the bridge.
_BUILD_TIMEOUT_SECONDS = 60.0

# Filename patterns produced by ``agent_build.sh``. We glob for either, in
# insertion order; ``inner`` matches the per-version artifact, ``outer``
# the per-release artifact.
_INNER_TARBALL_GLOB = "*.tar.gz"

# Project root is the parent of the ``frontend/bridge/`` package, computed the same
# way as in :mod:`config` so the resolved ``agent_build.sh`` path is stable
# regardless of the caller's cwd.
_PROJECT_ROOT = Path(__file__).resolve().parent.parent.parent.parent
_AGENT_BUILD_SH = _PROJECT_ROOT / "publish" / "agent_build.sh"


# --- daemon helpers -------------------------------------------------------


async def _try_hot_reload_agent(
    package_name: str,
    client: DaemonClient | None,
) -> None:
    """Hot-reload an agent package in the daemon **only if already loaded**.

    If the agent is not registered in the daemon, this is a no-op — the
    user can load it manually via the "加载到守护进程" button.

    This is a best-effort operation:
    - If the daemon client is ``None`` or not connected → skip silently.
    - If the daemon returns any error → log a warning, do NOT throw.
    This guarantees that a file-system operation never fails because the
    daemon is unavailable or the agent definition is incomplete.
    """
    if client is None or not client.is_connected:
        logger.debug("Daemon not connected, skipping hot-reload for '%s'", package_name)
        return

    # Check if the agent is already loaded in the daemon
    try:
        agents = await client.agent_list()
    except DaemonError as exc:
        logger.debug("agent.list failed, skipping hot-reload for '%s': %s", package_name, exc)
        return

    # Match by package_name against agent id or name
    matched_id: str | None = None
    agent_list = agents if isinstance(agents, list) else agents.get("agents", [])
    for agent in agent_list:
        agent_id = agent.get("agent_id", "") or agent.get("id", "")
        agent_name = agent.get("name", "")
        if agent_id == package_name or agent_name == package_name:
            matched_id = agent_id
            break

    if matched_id is None:
        logger.debug(
            "Agent '%s' not loaded in daemon, hot-reload skipped "
            "(load it manually via '加载到守护进程')",
            package_name,
        )
        return

    # Agent is loaded → unload then reload
    package_path = str(settings.agent_packages_path / package_name)
    try:
        await client.agent_unload(matched_id)
        logger.info("Agent '%s' unloaded from daemon", matched_id)
    except DaemonError as exc:
        logger.warning("Failed to unload agent '%s': %s", matched_id, exc)
        return

    try:
        result = await client.agent_load(package_path)
        logger.info("Agent '%s' hot-reloaded into daemon: %s", package_name, result)
    except DaemonError as exc:
        logger.warning(
            "Could not hot-reload agent '%s' after update: %s", package_name, exc,
        )


# --- helpers ----------------------------------------------------------------


def _package_source_dir(name: str) -> Path:
    """Resolve the on-disk source tree for an agent package.

    Rejects ``..`` segments and any name that escapes the agent packages
    directory. Returns the absolute path; existence is left to the caller
    to assert (so 404 and 200 paths can differ).
    """
    base = settings.agent_packages_path
    candidate = (base / name).resolve()
    base_resolved = base.resolve()
    try:
        candidate.relative_to(base_resolved)
    except ValueError as exc:
        raise HTTPException(
            status_code=status.HTTP_403_FORBIDDEN,
            detail=f"Invalid package name: {name!r}",
        ) from exc
    return candidate


def _package_dist_dir(name: str) -> Path:
    """Return ``{package_source}/dist`` — the per-package output directory.

    Mirrors the convention used by T20 (the daemon-side loader) and keeps
    every build artefact scoped under its own package so they can be
    downloaded/inspected without leaking across packages.
    """
    return _package_source_dir(name) / "dist"


def _list_existing_tarballs(dist_dir: Path) -> list[Path]:
    """Return existing ``.tar.gz`` files in ``dist_dir`` sorted by mtime.

    Sorted oldest-first so the caller can pick the most-recent one with
    ``[-1]`` if it wants the "latest" artefact. Files that disappeared
    between glob and stat are silently skipped (race-safe enough for a
    build-once endpoint).
    """
    if not dist_dir.is_dir():
        return []
    files = [p for p in dist_dir.glob(_INNER_TARBALL_GLOB) if p.is_file()]
    files.sort(key=lambda p: p.stat().st_mtime)
    return files


def _sha256_of(path: Path) -> str:
    """Compute the hex sha256 of a file by streaming in 64 KiB chunks."""
    digest = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(64 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


# ---------------------------------------------------------------------------
# T5: filesystem CRUD layer — request models, path safety, async helpers
# ---------------------------------------------------------------------------


class FileWriteRequest(BaseModel):
    """Body for PUT /file/{path} — overwrite or create a file with content."""

    content: str = Field(..., description="Full UTF-8 file content.")


class FileRenameRequest(BaseModel):
    """Body for POST /file/{path}/rename."""

    new_name: str = Field(..., description="New file name (no path separators).")


class DirectoryCreateRequest(BaseModel):
    """Body for POST /dir/{path} — kept as a stable contract for future options."""

    model_config = {"extra": "forbid"}


class PackageCreateRequest(BaseModel):
    """Name for a new empty agent package."""

    name: str = Field(..., min_length=1, max_length=128, description="Package name.")


def _validate_package_name(name: str) -> str:
    """Validate the ``{name}`` URL segment for the package itself."""
    if not name or name in (".", ".."):
        raise HTTPException(
            status_code=status.HTTP_403_FORBIDDEN, detail="Invalid package name"
        )
    if "/" in name or "\\" in name or "\x00" in name:
        raise HTTPException(
            status_code=status.HTTP_403_FORBIDDEN, detail="Invalid package name"
        )
    if name.startswith("."):
        raise HTTPException(
            status_code=status.HTTP_403_FORBIDDEN, detail="Invalid package name"
        )
    return name


def _validate_relative_path(rel_path: str) -> List[str]:
    """Validate a user-supplied relative path for safety.

    Rejects absolute paths, ``..`` segments, NUL bytes, backslashes, and
    empty components. Returns the path split into clean segments preserving
    forward-slash hierarchy.

    Raises 403 on any violation — no FS access happens before this check.
    """
    if rel_path is None:
        raise HTTPException(
            status_code=status.HTTP_403_FORBIDDEN, detail="Invalid path"
        )
    if "\x00" in rel_path or "\\" in rel_path:
        raise HTTPException(
            status_code=status.HTTP_403_FORBIDDEN, detail="Invalid path characters"
        )
    if rel_path.startswith("/"):
        raise HTTPException(
            status_code=status.HTTP_403_FORBIDDEN,
            detail="Absolute paths are not allowed",
        )
    parts = [p for p in rel_path.split("/") if p]
    if any(p == ".." for p in parts):
        raise HTTPException(
            status_code=status.HTTP_403_FORBIDDEN,
            detail="Path traversal is not allowed",
        )
    if not parts:
        raise HTTPException(
            status_code=status.HTTP_403_FORBIDDEN, detail="Empty path is not allowed"
        )
    return parts


def _resolve_within(package_dir: Path, segments: List[str]) -> Path:
    """Resolve ``segments`` under ``package_dir`` and verify containment.

    Symlinks that escape the sandbox are caught here via the
    ``is_relative_to`` check after ``Path.resolve()``. This is the second
    line of defense behind ``_validate_relative_path`` (I-1, I-2).
    """
    candidate = package_dir.joinpath(*segments)
    resolved_pkg = package_dir.resolve()
    try:
        resolved = candidate.resolve(strict=False)
    except OSError as exc:
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST,
            detail=f"Path resolution failed: {exc}",
        ) from exc
    if not resolved.is_relative_to(resolved_pkg):
        raise HTTPException(
            status_code=status.HTTP_403_FORBIDDEN,
            detail="Path escapes package directory",
        )
    return resolved


def _package_dir(name: str) -> Path:
    """Return the absolute package directory, validating the name."""
    safe_name = _validate_package_name(name)
    base = settings.agent_packages_path
    pkg = base / safe_name
    if not pkg.exists() or not pkg.is_dir():
        raise HTTPException(
            status_code=status.HTTP_404_NOT_FOUND,
            detail=f"Package '{safe_name}' not found",
        )
    return pkg


async def _to_thread(func, *args, **kwargs):
    """Run a sync callable in the asyncio default executor."""
    loop = asyncio.get_running_loop()
    return await loop.run_in_executor(None, lambda: func(*args, **kwargs))


def _list_packages_sync() -> List[PackageInfo]:
    """Enumerate immediate subdirectories of the agent packages root."""
    base = settings.agent_packages_path
    if not base.exists() or not base.is_dir():
        return []
    out: List[PackageInfo] = []
    for child in sorted(base.iterdir(), key=lambda p: p.name):
        if not child.is_dir() or child.name.startswith("."):
            continue
        try:
            file_count = sum(1 for p in child.rglob("*") if p.is_file())
            mtime = child.stat().st_mtime
            last_modified = datetime.fromtimestamp(
                mtime, tz=timezone.utc
            ).isoformat(timespec="seconds")
        except OSError as exc:
            logger.warning("Skipping package %s due to error: %s", child, exc)
            continue
        out.append(
            PackageInfo(
                name=child.name,
                path=str(child.resolve()),
                file_count=file_count,
                last_modified=last_modified,
            )
        )
    return out


def _build_tree_sync(pkg_dir: Path) -> List[FileNode]:
    """Recursively build a sorted file tree rooted at ``pkg_dir``.

    Directories come first, then files; both groups sorted alphabetically
    by name. Hidden entries (dotfiles) are included because they're part
    of the package.
    """

    def _node(p: Path) -> FileNode:
        rel = p.relative_to(pkg_dir)
        if p.is_dir():
            children = sorted(
                (c for c in p.iterdir()),
                key=lambda c: (not c.is_dir(), c.name),
            )
            return FileNode(
                name=p.name,
                path=str(rel),
                type="dir",
                children=[_node(c) for c in children],
            )
        return FileNode(name=p.name, path=str(rel), type="file")

    children = sorted(
        (c for c in pkg_dir.iterdir()),
        key=lambda c: (not c.is_dir(), c.name),
    )
    return [_node(c) for c in children]


def _write_file_sync(target: Path, content: str) -> None:
    """Write ``content`` to ``target``, creating parents if needed."""
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(content, encoding="utf-8")


def _create_empty_file_sync(target: Path) -> None:
    """Create ``target`` as an empty file. Fails if it already exists."""
    target.parent.mkdir(parents=True, exist_ok=True)
    fd = target.open("x", encoding="utf-8")
    fd.close()


def _delete_file_sync(target: Path) -> None:
    """Delete a file. Refuses to remove directories via this endpoint."""
    if target.is_dir() and not target.is_symlink():
        raise FileExistsError(
            "Target is a directory; use DELETE /dir instead"
        )
    target.unlink()


def _rename_sync(src: Path, new_name: str) -> Path:
    """Rename a file within the same parent directory."""
    if (
        not new_name
        or "/" in new_name
        or "\\" in new_name
        or "\x00" in new_name
    ):
        raise ValueError("new_name must be a bare file name")
    dst = src.with_name(new_name)
    if dst.exists():
        raise FileExistsError(f"Target '{new_name}' already exists")
    src.rename(dst)
    return dst


def _make_dir_sync(target: Path) -> None:
    """Create ``target`` as a directory, including parents."""
    target.mkdir(parents=True, exist_ok=False)


def _delete_dir_sync(target: Path) -> None:
    """Delete an empty directory."""
    target.rmdir()


def _make_scripts_executable(pkg_dir: Path) -> None:
    """Recursively set execute permission on ``.sh`` files under ``pkg_dir``.

    ``tarfile`` with ``filter="data"`` (Python ≥3.12) strips the executable
    bit from all regular files as a security measure, and ``ZipFile`` does
    not preserve Unix permissions at all.  Without this step, shell-script
    workflow nodes (``execution_mode: shell_script``) would fail with
    ``exit code 126`` / ``Permission denied`` at runtime.
    """
    for f in pkg_dir.rglob("*"):
        if f.is_file() and f.suffix in (".sh", ".bash"):
            mode = f.stat().st_mode
            # Add owner/group/other execute if the file is at least owner-readable
            if mode & stat.S_IRUSR:
                f.chmod(mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)


async def _run_agent_build(source_dir: Path, output_dir: Path) -> None:
    """Invoke ``publish/agent_build.sh`` against ``source_dir`` → ``output_dir``.

    The shell script is launched with ``bash`` (not just ``sh``) because
    ``set -e`` plus bash-isms (``<<<``) are part of the script. ``cwd`` is
    pinned to the project root so the script's own ``SCRIPT_DIR``/``PROJECT_ROOT``
    resolution agrees with ours.

    On non-zero exit, raises ``HTTPException(500)`` with the captured stderr
    so the caller sees *why* the build failed instead of a generic message.
    """
    if not _AGENT_BUILD_SH.is_file():
        raise HTTPException(
            status_code=status.HTTP_500_INTERNAL_SERVER_ERROR,
            detail=f"agent_build.sh not found at {_AGENT_BUILD_SH}",
        )

    output_dir.mkdir(parents=True, exist_ok=True)

    cmd = [
        "bash",
        str(_AGENT_BUILD_SH),
        "build",
        str(source_dir),
        str(output_dir),
    ]
    logger.info("Packaging: invoking %s", " ".join(cmd))

    try:
        proc = await asyncio.create_subprocess_exec(
            *cmd,
            cwd=str(_PROJECT_ROOT),
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE,
        )
    except FileNotFoundError as exc:
        # bash not on PATH — extremely unusual on POSIX, but be explicit.
        raise HTTPException(
            status_code=status.HTTP_500_INTERNAL_SERVER_ERROR,
            detail=f"bash not available on PATH: {exc}",
        ) from exc

    try:
        stdout_b, stderr_b = await asyncio.wait_for(
            proc.communicate(), timeout=_BUILD_TIMEOUT_SECONDS
        )
    except asyncio.TimeoutError as exc:
        proc.kill()
        await proc.wait()
        raise HTTPException(
            status_code=status.HTTP_504_GATEWAY_TIMEOUT,
            detail=(
                f"agent_build.sh timed out after {_BUILD_TIMEOUT_SECONDS}s"
            ),
        ) from exc

    stdout = stdout_b.decode("utf-8", errors="replace")
    stderr = stderr_b.decode("utf-8", errors="replace")
    if proc.returncode != 0:
        logger.error(
            "agent_build.sh failed (rc=%s) stdout=%s stderr=%s",
            proc.returncode,
            stdout,
            stderr,
        )
        raise HTTPException(
            status_code=status.HTTP_500_INTERNAL_SERVER_ERROR,
            detail={
                "message": "agent_build.sh failed",
                "exit_code": proc.returncode,
                "stdout": stdout,
                "stderr": stderr,
            },
        )

    logger.info("agent_build.sh succeeded: %s", stdout.strip())


# ---------------------------------------------------------------------------
# Package lifecycle management (create, upload)
# ---------------------------------------------------------------------------


@router.post(
    "/create",
    status_code=status.HTTP_201_CREATED,
    summary="Create a new empty agent package",
)
async def create_package(body: PackageCreateRequest) -> PackageInfo:
    """Create an empty package directory under ``agent_packages_path``.

    The directory is created with zero files so the caller can populate it
    via the file CRUD endpoints. 409 if a package with that name already
    exists.
    """
    safe_name = _validate_package_name(body.name)
    base = settings.agent_packages_path
    pkg_dir = base / safe_name
    try:
        pkg_dir.resolve().relative_to(base.resolve())
    except ValueError as exc:
        raise HTTPException(
            status_code=status.HTTP_403_FORBIDDEN,
            detail=f"Invalid package name: {safe_name!r}",
        ) from exc
    if pkg_dir.exists():
        raise HTTPException(
            status_code=status.HTTP_409_CONFLICT,
            detail=f"Package '{safe_name}' already exists",
        )
    pkg_dir.mkdir(parents=True, exist_ok=False)
    now = datetime.now(tz=timezone.utc).isoformat(timespec="seconds")
    logger.info("Created package '%s' at %s", safe_name, pkg_dir)
    return PackageInfo(
        name=safe_name,
        path=str(pkg_dir.resolve()),
        file_count=0,
        last_modified=now,
    )


_ALLOWED_UPLOAD_SUFFIXES = (".zip", ".tar.gz", ".tgz")
_UPDATE_MODES = {"full", "incremental"}


@router.post(
    "/upload",
    status_code=status.HTTP_201_CREATED,
    summary="Upload & extract an agent package archive",
)
async def upload_package(
    file: UploadFile = File(..., description="Archive file (.zip / .tar.gz)."),
    name: str | None = None,
) -> PackageInfo:
    """Upload a ``.zip`` or ``.tar.gz`` archive and extract it as a new package.

    The HTTP request must be ``multipart/form-data`` with:

    * ``file`` — the archive binary.
    * ``name`` (optional) — the package name. When omitted it is inferred
      from the archive filename (without extension).

    Returns the ``PackageInfo`` of the newly created package.
    """
    # --- validate file extension ------------------------------------------------
    if not file.filename:
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST,
            detail="Upload filename is required",
        )

    lower_name = file.filename.lower()
    if not lower_name.endswith(_ALLOWED_UPLOAD_SUFFIXES):
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST,
            detail=(
                f"Unsupported archive format: {file.filename!r}. "
                f"Allowed: {', '.join(_ALLOWED_UPLOAD_SUFFIXES)}"
            ),
        )

    is_tar = lower_name.endswith((".tar.gz", ".tgz"))
    is_zip = lower_name.endswith(".zip")

    # --- infer/determine package name -------------------------------------------
    if name:
        _validate_package_name(name)
    else:
        # Derive from filename: strip known extensions
        stem = file.filename.rsplit(".", 2 if is_tar else 1)[0]
        name = stem

    safe_name = _validate_package_name(name)
    base = settings.agent_packages_path
    pkg_dir = base / safe_name
    try:
        pkg_dir.resolve().relative_to(base.resolve())
    except ValueError as exc:
        raise HTTPException(
            status_code=status.HTTP_403_FORBIDDEN,
            detail=f"Invalid package name: {safe_name!r}",
        ) from exc
    if pkg_dir.exists():
        raise HTTPException(
            status_code=status.HTTP_409_CONFLICT,
            detail=f"Package '{safe_name}' already exists",
        )

    # --- save uploaded file to a temp location ----------------------------------
    try:
        suffix = ".tar.gz" if is_tar else ".zip"
        with tempfile.NamedTemporaryFile(delete=False, suffix=suffix) as tmp:
            tmp_path = Path(tmp.name)
            content = await file.read()
            tmp.write(content)
    except OSError as exc:
        raise HTTPException(
            status_code=status.HTTP_500_INTERNAL_SERVER_ERROR,
            detail=f"Failed to save uploaded file: {exc}",
        ) from exc

    # --- extract archive into the package directory -----------------------------
    try:
        if is_tar:
            with tarfile.open(tmp_path, "r:gz") as tf:
                tf.extractall(path=pkg_dir, filter="data")
        elif is_zip:
            with zipfile.ZipFile(tmp_path, "r") as zf:
                zf.extractall(path=pkg_dir)
    except (tarfile.TarError, zipfile.BadZipFile, OSError) as exc:
        # Clean up the partial directory on failure
        shutil.rmtree(pkg_dir, ignore_errors=True)
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST,
            detail=f"Failed to extract archive: {exc}",
        ) from exc
    finally:
        tmp_path.unlink(missing_ok=True)

    # --- restore execute permission on shell scripts ---------------------------
    # tarfile's "data" filter strips execute bits; zip doesn't preserve them.
    _make_scripts_executable(pkg_dir)

    # --- build PackageInfo ------------------------------------------------------
    file_count = 0
    try:
        file_count = sum(1 for p in pkg_dir.rglob("*") if p.is_file())
    except OSError:
        pass

    now = datetime.now(tz=timezone.utc).isoformat(timespec="seconds")
    logger.info(
        "Uploaded & extracted package '%s' (%d files) from archive %s",
        safe_name,
        file_count,
        file.filename,
    )
    return PackageInfo(
        name=safe_name,
        path=str(pkg_dir.resolve()),
        file_count=file_count,
        last_modified=now,
    )


@router.put(
    "/{name}/update",
    status_code=status.HTTP_200_OK,
    summary="Update an existing agent package from an archive",
)
async def update_package(
    name: str,
    file: UploadFile = File(..., description="Archive file (.zip / .tar.gz)."),
    mode: str = Form("incremental"),
    client: DaemonClient = Depends(get_daemon_client),
) -> PackageInfo:
    """Update an existing package by uploading an archive.

    Two modes are supported via the ``mode`` query/form parameter:

    * **full** — delete the existing package contents **before** extracting.
      The directory itself (and any ``dist/`` sub-directory) is preserved
      so built artifacts are not lost.

    * **incremental** (default) — extract the archive on top of the existing
      files. New files are added; existing files with the same relative path
      are overwritten; files not present in the archive are left untouched.
      This is the safe default for quick updates.

    The HTTP request must be ``multipart/form-data`` with:

    * ``file`` — the archive binary.
    * ``mode`` (optional, default ``"incremental"``) — ``"full"`` or ``"incremental"``.

    Returns the ``PackageInfo`` of the updated package.
    """
    # --- validate file extension ------------------------------------------------
    if not file.filename:
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST,
            detail="Upload filename is required",
        )

    lower_name = file.filename.lower()
    if not lower_name.endswith(_ALLOWED_UPLOAD_SUFFIXES):
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST,
            detail=(
                f"Unsupported archive format: {file.filename!r}. "
                f"Allowed: {', '.join(_ALLOWED_UPLOAD_SUFFIXES)}"
            ),
        )

    is_tar = lower_name.endswith((".tar.gz", ".tgz"))
    is_zip = lower_name.endswith(".zip")

    # --- validate mode ----------------------------------------------------------
    mode_lower = mode.strip().lower()
    if mode_lower not in _UPDATE_MODES:
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST,
            detail=f"Invalid update mode: {mode!r}. Allowed: {', '.join(sorted(_UPDATE_MODES))}",
        )

    # --- validate package name & existence --------------------------------------
    safe_name = _validate_package_name(name)
    pkg_dir = _package_dir(safe_name)
    if not pkg_dir.is_dir():
        raise HTTPException(
            status_code=status.HTTP_404_NOT_FOUND,
            detail=f"Package '{safe_name}' not found",
        )

    # --- save uploaded file to a temp location ----------------------------------
    try:
        suffix = ".tar.gz" if is_tar else ".zip"
        with tempfile.NamedTemporaryFile(delete=False, suffix=suffix) as tmp:
            tmp_path = Path(tmp.name)
            content = await file.read()
            tmp.write(content)
    except OSError as exc:
        raise HTTPException(
            status_code=status.HTTP_500_INTERNAL_SERVER_ERROR,
            detail=f"Failed to save uploaded file: {exc}",
        ) from exc

    # --- full mode: clear existing contents (preserve directory + dist) ---------
    if mode_lower == "full":
        dist_dir = pkg_dir / "dist"
        try:
            for child in pkg_dir.iterdir():
                if child.name == "dist":
                    continue  # preserve built artefacts
                if child.is_dir():
                    shutil.rmtree(child)
                else:
                    child.unlink()
        except OSError as exc:
            raise HTTPException(
                status_code=status.HTTP_500_INTERNAL_SERVER_ERROR,
                detail=f"Failed to clear package directory: {exc}",
            ) from exc
        logger.info("Cleared package '%s' for full update", safe_name)

    # --- extract archive into the package directory -----------------------------
    try:
        if is_tar:
            with tarfile.open(tmp_path, "r:gz") as tf:
                tf.extractall(path=pkg_dir, filter="data")
        elif is_zip:
            with zipfile.ZipFile(tmp_path, "r") as zf:
                zf.extractall(path=pkg_dir)
    except (tarfile.TarError, zipfile.BadZipFile, OSError) as exc:
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST,
            detail=f"Failed to extract archive: {exc}",
        ) from exc
    finally:
        tmp_path.unlink(missing_ok=True)

    # --- restore execute permission on shell scripts ---------------------------
    # tarfile's "data" filter strips execute bits; zip doesn't preserve them.
    _make_scripts_executable(pkg_dir)

    # --- build PackageInfo ------------------------------------------------------
    file_count = 0
    try:
        file_count = sum(1 for p in pkg_dir.rglob("*") if p.is_file())
    except OSError:
        pass

    now = datetime.now(tz=timezone.utc).isoformat(timespec="seconds")
    logger.info(
        "Updated package '%s' (mode=%s, %d files) from archive %s",
        safe_name,
        mode_lower,
        file_count,
        file.filename,
    )

    # --- hot-reload (only if already loaded in daemon) ---------------------------
    await _try_hot_reload_agent(safe_name, client)

    return PackageInfo(
        name=safe_name,
        path=str(pkg_dir.resolve()),
        file_count=file_count,
        last_modified=now,
    )


# ---------------------------------------------------------------------------
# T5 CRUD surface
# ---------------------------------------------------------------------------


@router.get("", response_model=List[PackageInfo], summary="List agent packages on disk")
async def list_packages() -> List[PackageInfo]:
    """Return every directory under ``agent_packages_path`` as a package.

    Each entry mirrors the ``PackageInfo`` schema in :mod:`models`: name,
    absolute path, recursive file count, and ISO-8601 last-modified.

    Hidden directories (leading ``.``) and files are excluded so e.g.
    ``.git`` folders don't appear as fake packages.
    """
    return await _to_thread(_list_packages_sync)


@router.get(
    "/{name}/tree",
    response_model=List[FileNode],
    response_model_exclude_none=True,
    summary="Recursive file tree of a package",
)
async def get_package_tree(
    name: str = PathParam(..., description="Package directory name"),
) -> List[FileNode]:
    """Return the recursive file tree of a single package."""
    pkg_dir = _package_dir(name)
    return await _to_thread(_build_tree_sync, pkg_dir)


@router.get("/{name}/file/{file_path:path}", summary="Read file content")
async def read_file(
    name: str,
    file_path: str = PathParam(..., description="Relative file path"),
) -> dict[str, str]:
    """Read the UTF-8 contents of a file inside the package."""
    pkg_dir = _package_dir(name)
    segments = _validate_relative_path(file_path)
    target = _resolve_within(pkg_dir, segments)
    if not target.exists():
        raise HTTPException(
            status_code=status.HTTP_404_NOT_FOUND, detail="File not found"
        )
    if target.is_dir():
        raise HTTPException(
            status_code=status.HTTP_409_CONFLICT, detail="Path is a directory"
        )

    content = await _to_thread(lambda: target.read_text(encoding="utf-8"))
    return {"path": file_path, "content": content}


@router.put(
    "/{name}/file/{file_path:path}",
    status_code=status.HTTP_200_OK,
    summary="Write file content (create or overwrite)",
)
async def write_file(
    body: FileWriteRequest,
    name: str,
    file_path: str = PathParam(..., description="Relative file path"),
) -> dict[str, Any]:
    """Create or overwrite a file with the supplied UTF-8 content."""
    pkg_dir = _package_dir(name)
    segments = _validate_relative_path(file_path)
    target = _resolve_within(pkg_dir, segments)
    await _to_thread(_write_file_sync, target, body.content)
    logger.info("Wrote %d bytes to %s/%s", len(body.content), name, file_path)
    return {"path": file_path, "bytes": len(body.content.encode("utf-8"))}


@router.post(
    "/{name}/file/{file_path:path}/rename",
    status_code=status.HTTP_200_OK,
    summary="Rename a file within its parent directory",
)
async def rename_file(
    body: FileRenameRequest,
    name: str,
    file_path: str = PathParam(..., description="Relative file path"),
) -> dict[str, str]:
    """Rename a file within its parent directory."""
    pkg_dir = _package_dir(name)
    segments = _validate_relative_path(file_path)
    target = _resolve_within(pkg_dir, segments)
    if not target.exists():
        raise HTTPException(
            status_code=status.HTTP_404_NOT_FOUND, detail="File not found"
        )
    if target.is_dir():
        raise HTTPException(
            status_code=status.HTTP_409_CONFLICT,
            detail="Use a directory operation to rename directories",
        )

    def _do_rename() -> Path:
        return _rename_sync(target, body.new_name)

    try:
        new_path = await _to_thread(_do_rename)
    except ValueError as exc:
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST, detail=str(exc)
        ) from exc
    except FileExistsError as exc:
        raise HTTPException(
            status_code=status.HTTP_409_CONFLICT, detail=str(exc)
        ) from exc
    rel = new_path.relative_to(pkg_dir.resolve())
    return {"old_path": file_path, "new_path": str(rel)}


@router.post(
    "/{name}/file/{file_path:path}",
    status_code=status.HTTP_201_CREATED,
    summary="Create a new empty file",
)
async def create_file(
    name: str,
    file_path: str = PathParam(..., description="Relative file path"),
) -> dict[str, str]:
    """Create a new empty file. 409 if it already exists."""
    pkg_dir = _package_dir(name)
    segments = _validate_relative_path(file_path)
    target = _resolve_within(pkg_dir, segments)
    try:
        await _to_thread(_create_empty_file_sync, target)
    except FileExistsError as exc:
        raise HTTPException(
            status_code=status.HTTP_409_CONFLICT, detail=str(exc)
        ) from exc
    return {"path": file_path}


@router.delete(
    "/{name}/file/{file_path:path}",
    status_code=status.HTTP_204_NO_CONTENT,
    summary="Delete a file",
)
async def delete_file(
    name: str,
    file_path: str = PathParam(..., description="Relative file path"),
) -> None:
    """Delete a file. 404 if missing; 409 if target is a directory."""
    pkg_dir = _package_dir(name)
    segments = _validate_relative_path(file_path)
    target = _resolve_within(pkg_dir, segments)
    try:
        await _to_thread(_delete_file_sync, target)
    except FileNotFoundError as exc:
        raise HTTPException(
            status_code=status.HTTP_404_NOT_FOUND, detail="File not found"
        ) from exc
    except FileExistsError as exc:
        raise HTTPException(
            status_code=status.HTTP_409_CONFLICT, detail=str(exc)
        ) from exc
    return None


@router.post(
    "/{name}/dir/{dir_path:path}",
    status_code=status.HTTP_201_CREATED,
    summary="Create a new directory",
)
async def create_directory(
    name: str,
    dir_path: str = PathParam(..., description="Relative directory path"),
    _body: DirectoryCreateRequest = DirectoryCreateRequest(),
) -> dict[str, str]:
    """Create a new directory (parents created on demand). 409 if it exists."""
    pkg_dir = _package_dir(name)
    segments = _validate_relative_path(dir_path)
    target = _resolve_within(pkg_dir, segments)
    try:
        await _to_thread(_make_dir_sync, target)
    except FileExistsError as exc:
        raise HTTPException(
            status_code=status.HTTP_409_CONFLICT, detail=str(exc)
        ) from exc
    return {"path": dir_path}


@router.delete(
    "/{name}/dir/{dir_path:path}",
    status_code=status.HTTP_204_NO_CONTENT,
    summary="Delete an empty directory",
)
async def delete_directory(
    name: str,
    dir_path: str = PathParam(..., description="Relative directory path"),
) -> None:
    """Delete an empty directory. 409 if non-empty; 404 if missing."""
    pkg_dir = _package_dir(name)
    segments = _validate_relative_path(dir_path)
    target = _resolve_within(pkg_dir, segments)
    if not target.exists():
        raise HTTPException(
            status_code=status.HTTP_404_NOT_FOUND, detail="Directory not found"
        )
    if not target.is_dir():
        raise HTTPException(
            status_code=status.HTTP_409_CONFLICT, detail="Path is not a directory"
        )
    try:
        items = await _to_thread(list, target.iterdir())
    except OSError as exc:
        raise HTTPException(
            status_code=status.HTTP_500_INTERNAL_SERVER_ERROR,
            detail=f"Cannot list directory: {exc}",
        ) from exc
    if items:
        raise HTTPException(
            status_code=status.HTTP_409_CONFLICT,
            detail=(
                f"Directory not empty (contains {len(items)} entries); "
                "recursive delete is not supported"
            ),
        )
    try:
        await _to_thread(_delete_dir_sync, target)
    except OSError as exc:
        raise HTTPException(
            status_code=status.HTTP_500_INTERNAL_SERVER_ERROR,
            detail=f"Delete failed: {exc}",
        ) from exc
    return None


# ---------------------------------------------------------------------------
# T8 packaging surface
# ---------------------------------------------------------------------------


def _tarball_info(tarball: Path) -> dict[str, Any]:
    """Build the JSON envelope for a single tarball on disk."""
    stat = tarball.stat()
    return {
        "path": str(tarball),
        "name": tarball.name,
        "size": stat.st_size,
        "checksum": _sha256_of(tarball),
        "last_modified": stat.st_mtime,
    }


@router.post(
    "/{name}/package",
    summary="Build a deployable tarball for an agent package",
)
async def build_package(name: str) -> JSONResponse:
    """Run ``agent_build.sh`` and return metadata for the produced tarball.

    The script writes two artifacts to ``{package_source}/dist``:

    * the *inner* per-version tarball ``{Agent}_{agt_ver}.tar.gz``
    * the *outer* per-release tarball ``{Agent}_{pkg_type}_{pkg_ver}.tar.gz``

    We pick the most recently modified one (by mtime) as "the build output"
    and report its size + sha256. If both have the same mtime, whichever
    lands last in the glob wins.
    """
    source_dir = _package_source_dir(name)
    if not source_dir.is_dir():
        raise HTTPException(
            status_code=status.HTTP_404_NOT_FOUND,
            detail=f"Agent package not found: {name!r}",
        )
    if not (source_dir / "package.json").is_file():
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST,
            detail=(
                f"Agent package {name!r} is missing package.json; "
                "refusing to build"
            ),
        )

    dist_dir = _package_dist_dir(name)
    # Clean stale artefacts so callers always get a build produced by *this*
    # call rather than something left over from a previous run.
    if dist_dir.exists():
        shutil.rmtree(dist_dir)
    dist_dir.mkdir(parents=True, exist_ok=True)

    await _run_agent_build(source_dir, dist_dir)

    tarballs = _list_existing_tarballs(dist_dir)
    if not tarballs:
        # Build claimed success but no tarball — treat as a hard failure so
        # the caller doesn't receive an empty envelope.
        raise HTTPException(
            status_code=status.HTTP_500_INTERNAL_SERVER_ERROR,
            detail=(
                "agent_build.sh exited 0 but no .tar.gz was produced in "
                f"{dist_dir}"
            ),
        )

    chosen = tarballs[-1]
    return JSONResponse(content=_tarball_info(chosen))


@router.get(
    "/{name}/package",
    summary="Get metadata for an already-built tarball",
)
async def get_package(name: str) -> JSONResponse:
    """Return the same envelope as POST but without rebuilding.

    Returns 404 when no tarball has been built yet — callers must invoke
    POST first. This keeps "did the build run?" visible to the UI without
    having to inspect the filesystem.
    """
    _package_source_dir(name)  # enforces 403 on path traversal
    tarballs = _list_existing_tarballs(_package_dist_dir(name))
    if not tarballs:
        raise HTTPException(
            status_code=status.HTTP_404_NOT_FOUND,
            detail=(
                f"No built tarball for package {name!r}; "
                "call POST /api/packages/{name}/package first"
            ),
        )
    return JSONResponse(content=_tarball_info(tarballs[-1]))


@router.get(
    "/{name}/package/download",
    summary="Download the most-recent built tarball",
)
async def download_package(name: str) -> FileResponse:
    """Stream the latest tarball back to the caller.

    The filename advertised to the browser is the tarball's on-disk name
    (e.g. ``BasicAgent_1.0.0.tar.gz``) so the downloaded artefact is
    self-describing. We do **not** set ``Content-Disposition: attachment``
    explicitly; ``FileResponse`` defaults to ``inline`` and lets the
    browser decide.
    """
    _package_source_dir(name)  # enforces 403 on path traversal
    tarballs = _list_existing_tarballs(_package_dist_dir(name))
    if not tarballs:
        raise HTTPException(
            status_code=status.HTTP_404_NOT_FOUND,
            detail=(
                f"No built tarball for package {name!r}; "
                "call POST /api/packages/{name}/package first"
            ),
        )
    chosen = tarballs[-1]
    return FileResponse(
        path=chosen,
        media_type="application/gzip",
        filename=chosen.name,
    )
