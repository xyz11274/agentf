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
import json
import logging
import os
import shutil
import tarfile
import tempfile
import zipfile
from datetime import UTC, datetime
from pathlib import Path
from typing import Any

from fastapi import APIRouter, Depends, File, Form, HTTPException, UploadFile, status
from fastapi import Path as PathParam
from fastapi.responses import FileResponse, JSONResponse
from pydantic import BaseModel, Field

from client import DaemonClient, DaemonError
from config import settings
from dependencies import get_daemon_client
from models import FileNode, PackageInfo
from templates.agent_template.merger import (
    _make_scripts_executable,
    _reapply_template_files,
    merge_template_with_user_upload,
)

logger = logging.getLogger("agentflow.bridge.routes.packages")

router = APIRouter(prefix="/api/packages", tags=["packages"])


# --- §5.1/§5.2 setup 驱动执行（宿主侧）-----------------------------------
# Bridge 在 create/update 流程末验 setup-host.sh（幂等）。宿主侧初始化负责创建 .venv +
# 装依赖 + 装 agentflow-runtime + 写 sentinel（见设计文档 §5.4/§3.5）。
# 2026-08-11 分离：宿主执行 setup-host.sh；setup.sh 改为沙箱版（用户感知、上传沙箱，
# 由 template_admin.py snapshot 在沙箱预热时执行）。旧包无 setup-host.sh 时 fallback setup.sh。
# 失败不阻断主流程（只 WARN）——shell wrapper fallback 到系统 python3。
def _run_setup_sh(pkg_dir: Path) -> None:
    setup_script = pkg_dir / "setup-host.sh"
    script_kind = "setup-host.sh"
    if not setup_script.is_file():
        # 存量包可能只有旧 setup.sh（宿主版语义）——fallback 兼容，WARN 提示迁移
        legacy = pkg_dir / "setup.sh"
        if not legacy.is_file():
            logger.debug("setup-host.sh/setup.sh not found in %s, skip", pkg_dir)
            return
        setup_script = legacy
        script_kind = "setup.sh (legacy, 建议 update 一次以获取 setup-host.sh)"
    import os as _os
    import subprocess
    try:
        proc = subprocess.run(
            ["bash", str(setup_script)],
            cwd=str(pkg_dir),
            check=False,
            capture_output=True,
            text=True,
            timeout=300,
            env={**_os.environ, "AF_SETUP_BY": "bridge"},
        )
        if proc.returncode == 0:
            logger.info("%s ok pkg=%s", script_kind, pkg_dir.name)
        else:
            logger.warning(
                "%s failed pkg=%s rc=%s stderr=%s",
                script_kind, pkg_dir.name, proc.returncode, proc.stderr[:500] if proc.stderr else "",
            )
    except Exception as exc:  # noqa: BLE001
        logger.warning("%s invocation error pkg=%s: %s", script_kind, pkg_dir.name, exc)

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
    # 三态模型：daemon 只 load ③ workspace 实例；先同步 ②→③ 再 load ③，
    # 运行时数据不落 ② 固化态（agent-package-three-state.md §3.2）。
    from routes.agents import _sync_agent_package
    try:
        workspace_path = _sync_agent_package(package_name)
    except HTTPException as exc:
        logger.warning("hot_reload.sync_failed pkg=%s error=%s", package_name, exc.detail)
        return
    # setup.sh 在 ③ workspace 实例上执行（三态模型 I-3S-1：.venv/ 只落 ③）
    _run_setup_sh(workspace_path)
    package_path = str(workspace_path)
    try:
        await client.agent_unload(matched_id)
        logger.info("Agent '%s' unloaded from daemon", matched_id)
    except DaemonError as exc:
        logger.warning("Failed to unload agent '%s': %s", matched_id, exc)
        return

    # 注入默认配置到 .env（与 agents.py load_agent 路由一致）
    from routes.agents import _inject_agent_env_defaults
    _inject_agent_env_defaults(package_name)

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


def _validate_relative_path(rel_path: str) -> list[str]:
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


def _resolve_within(package_dir: Path, segments: list[str]) -> Path:
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


def _list_packages_sync() -> list[PackageInfo]:
    """Enumerate immediate subdirectories of the agent packages root."""
    base = settings.agent_packages_path
    if not base.exists() or not base.is_dir():
        return []
    out: list[PackageInfo] = []
    for child in sorted(base.iterdir(), key=lambda p: p.name):
        if not child.is_dir() or child.name.startswith("."):
            continue
        try:
            file_count = sum(1 for p in child.rglob("*") if p.is_file())
            mtime = child.stat().st_mtime
            last_modified = datetime.fromtimestamp(
                mtime, tz=UTC
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


def _build_tree_sync(pkg_dir: Path) -> list[FileNode]:
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


def _normalize_permissions(pkg_dir: Path) -> None:
    """Normalise file and directory permissions and owner under ``pkg_dir``.

    ``tarfile`` with ``filter="data"`` (Python ≥3.12) strips the executable
    bit from all regular files as a security measure, and ``ZipFile`` does
    not preserve Unix permissions at all.  Without this step, shell-script
    workflow nodes (``execution_mode: shell_script``) would fail with
    ``exit code 126`` / ``Permission denied`` at runtime, and directories
    with missing +x bits would be inaccessible.

    Applied permissions::

        Directories                  drwxr-xr-x  (755)
        Regular files                -rw-r--r--  (644)
        Shell scripts (.sh / .bash)  -rwxr-xr-x  (755)

    Owner is also normalised to the current process user so that files
    originating from an archive owned by (e.g.) ``root:root`` do not
    become inaccessible after extraction.  On non-POSIX platforms or when
    running as a non-root user, the chown silently succeeds or is skipped.

    Order matters: **chown first, then chmod**.  On Linux, a non-owner
    cannot ``chmod`` a file — if the archive carried a foreign uid/gid
    (e.g. ``root:root`` from a container-built tar), chmod would fail
    with ``EPERM`` (Operation not permitted).  By chown'ing first we
    become the owner and chmod succeeds.

    Failures on individual entries are logged as warnings (not silently
    swallowed) so operators can diagnose permission issues without the
    whole upload being blocked.
    """
    # ── Owner normalisation (先于 chmod，避免非属主无法 chmod) ──
    uid: int | None = None
    gid: int | None = None
    try:
        uid = os.getuid()
        gid = os.getgid()
    except AttributeError:
        pass  # non-POSIX (e.g. Windows — no os.getuid)

    # ── Permission bits + owner (单次遍历) ──
    for f in pkg_dir.rglob("*"):
        # Skip symlinks — chown/chmod on a symlink operates on its target,
        # which may escape the package directory (see P1 #3 in review).
        if f.is_symlink():
            continue

        # Owner normalisation
        if uid is not None:
            try:
                st = f.stat()
                if st.st_uid != uid or st.st_gid != gid:
                    os.chown(f, uid, gid)
            except OSError as exc:
                logger.warning(
                    "normalize: cannot chown %s to %d:%d — %s",
                    f, uid, gid, exc,
                )

        # Permission bits
        try:
            target = 0o755 if f.is_dir() or (f.is_file() and f.suffix in (".sh", ".bash")) else 0o644
            f.chmod(target)
        except OSError as exc:
            logger.warning(
                "normalize: cannot chmod %s to %o — %s",
                f, target, exc,
            )


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
    except TimeoutError as exc:
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
    now = datetime.now(tz=UTC).isoformat(timespec="seconds")
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

    # --- auto-fill/merge framework files from harness template -------------
    # Users only need to provide agent.md + their custom harness/ skills.
    # The harness template provides the generic agent runtime foundation:
    # tools (bash.py, read_file.py, ...), runner skills (skill-runner),
    # and node scripts (coordinator_decide.py, skill_executor.py, ...).
    #
    # Merge strategy:
    #   Single files (package.json, agent.json, workflows) → copy if missing
    #   Directories (harness/, nodes/) → recursive merge, user files win
    #
    # NOTE: template 路径走 settings.agent_template_path（单一真相，F5），
    # 与 update_package 的 _reapply_template_files 同源——configs 改一次两边都生效。
    # template 与 create-from-template 路由的默认值一致（"harness_agent"）。
    _TEMPLATE_DIR = settings.agent_template_path / "harness_agent"
    if _TEMPLATE_DIR.is_dir():

        def _merge_dir(src_dir: Path, dst_dir: Path) -> None:
            """Recursively copy src_dir into dst_dir. dst files win if exist."""
            for child in src_dir.iterdir():
                src_child = child
                dst_child = dst_dir / child.name
                if child.is_dir():
                    dst_child.mkdir(parents=True, exist_ok=True)
                    _merge_dir(src_child, dst_child)
                elif child.is_file() and not dst_child.exists():
                    shutil.copy2(src_child, dst_child)

        # Copy single framework files if missing
        for name in ("package.json", "agent.json",
                      "coordinator_workflow.json", "skill_workflow.json",
                      "dummy_pre_skill_workflow.json"):
            src = _TEMPLATE_DIR / name
            dst = pkg_dir / name
            if src.is_file() and not dst.exists():
                shutil.copy2(src, dst)
                logger.info("auto_fill file %s", name)

        # Merge harness/ and nodes/ (recursive, user files win)
        for name in ("harness", "nodes"):
            src = _TEMPLATE_DIR / name
            dst = pkg_dir / name
            if src.is_dir():
                dst.mkdir(parents=True, exist_ok=True)
                _merge_dir(src, dst)
                logger.info("auto_fill merge %s/", name)

        # Override package.json name with the user-provided package name
        pkg_json_path = pkg_dir / "package.json"
        if pkg_json_path.exists():
            try:
                pkg_data = json.loads(pkg_json_path.read_text(encoding="utf-8"))
                pkg_data["name"] = safe_name
                pkg_json_path.write_text(
                    json.dumps(pkg_data, indent=2, ensure_ascii=False) + "\n",
                    encoding="utf-8",
                )
            except (json.JSONDecodeError, OSError) as exc:
                logger.warning("auto_fill.name_override pkg=%s error=%s", safe_name, exc)

    # --- normalise file & directory permissions -------------------------------
    # tarfile's "data" filter strips execute bits; zip doesn't preserve them.
    # Directories missing +x are inaccessible; scripts without +x fail at runtime.
    _normalize_permissions(pkg_dir)

    # --- build PackageInfo ------------------------------------------------------
    file_count = 0
    try:
        file_count = sum(1 for p in pkg_dir.rglob("*") if p.is_file())
    except OSError:
        pass

    now = datetime.now(tz=UTC).isoformat(timespec="seconds")
    logger.info(
        "package_upload.complete name=%s files=%d archive=%s",
        safe_name, file_count, file.filename,
    )

    # 三态模型：setup.sh 不在 ② 固化态执行（避免 .venv/ 污染），
    # 改在 load_agent 时于 ③ workspace 实例上执行（I-3S-1）。

    return PackageInfo(
        name=safe_name,
        path=str(pkg_dir.resolve()),
        file_count=file_count,
        last_modified=now,
    )


# --- template-supported agent creation --------------------------------------


@router.post(
    "/create-from-template",
    status_code=status.HTTP_201_CREATED,
    summary="Create an agent by merging a user upload with a template",
)
async def create_from_template(
    file: UploadFile = File(..., description="User's agent archive (.zip / .tar.gz)."),
    name: str | None = Form(None),
    template: str = Form("harness_agent"),
) -> PackageInfo:
    """Upload a user's agent archive and merge it with the harness template.

    The merge is one-directional — the template provides runtime infrastructure
    (``harness/tools/``, ``nodes/databus*``, ``tests/``, …) while the user
    provides identity (``agent.md``, ``package.json``) and business logic
    (custom skills under ``harness/skills/``). See ``merger.py`` for the
    full ownership table.

    ``multipart/form-data`` fields:

    * ``file`` — the user's archive (.zip / .tar.gz). This should contain
      at minimum an ``agent.md`` with the agent's identity.
    * ``name`` (optional) — package name. Defaults to the archive stem.
    * ``template`` (optional) — template directory under ``agent_template_dir``.
      Default: ``"harness_agent"``.

    Returns the ``PackageInfo`` of the newly created, merged package.
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

    # --- infer / determine package name -----------------------------------------
    stem: str
    if name:
        _validate_package_name(name)
        stem = name
    else:
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

    # --- validate template ------------------------------------------------------
    tmpl_root = settings.agent_template_path
    tmpl_dir = tmpl_root / template
    if not tmpl_dir.is_dir():
        # List available templates for the error message
        available = ", ".join(
            d.name for d in tmpl_root.iterdir() if d.is_dir() and not d.name.startswith(".")
        ) if tmpl_root.is_dir() else "(none)"
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST,
            detail=(
                f"Template '{template}' not found in {tmpl_root.resolve()}. "
                f"Available: {available}"
            ),
        )

    # --- compute relative working_dir for workflow files -----------------------
    # Workflow files reference working_dir relative to the project root.
    # E.g. if agent_packages_dir = "examples/agent" and name = "my-agent",
    # the working_dir is "examples/agent/my-agent".
    # Falls back to bare package name when pkg_dir isn't under the project root
    # (e.g. in tests with temp directories).
    try:
        pkg_workdir = str(
            pkg_dir.resolve().relative_to(
                Path(__file__).resolve().parent.parent.parent.parent
            )
        )
    except ValueError:
        # Fall back to just the package name (test environments, …)
        pkg_workdir = safe_name
        logger.warning(
            "working_dir fallback to '%s' (pkg_dir not under project root: %s)",
            safe_name, pkg_dir.resolve(),
        )

    # --- read uploaded file content --------------------------------------------
    try:
        content = await file.read()
    except OSError as exc:
        raise HTTPException(
            status_code=status.HTTP_500_INTERNAL_SERVER_ERROR,
            detail=f"Failed to read uploaded file: {exc}",
        ) from exc

    # --- perform merge ---------------------------------------------------------
    try:
        merge_template_with_user_upload(
            template_dir=tmpl_dir.resolve(),
            user_archive=content,
            archive_filename=file.filename,
            package_name=safe_name,
            target_dir=pkg_dir,
            package_workdir=pkg_workdir,
        )
    except ValueError as exc:
        # Clean up partial directory on validation failure
        shutil.rmtree(pkg_dir, ignore_errors=True)
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST,
            detail=str(exc),
        ) from exc
    except OSError as exc:
        shutil.rmtree(pkg_dir, ignore_errors=True)
        raise HTTPException(
            status_code=status.HTTP_500_INTERNAL_SERVER_ERROR,
            detail=f"Filesystem error during merge: {exc}",
        ) from exc

    # --- normalise file & directory permissions -------------------------------
    _normalize_permissions(pkg_dir)

    # --- build PackageInfo ------------------------------------------------------
    file_count = 0
    try:
        file_count = sum(1 for p in pkg_dir.rglob("*") if p.is_file())
    except OSError:
        pass

    now = datetime.now(tz=UTC).isoformat(timespec="seconds")
    logger.info(
        "package_create_from_template.complete name=%s template=%s files=%d",
        safe_name, template, file_count,
    )

    # 三态模型：setup.sh 不在 ② 固化态执行（避免 .venv/ 污染），
    # 改在 load_agent 时于 ③ workspace 实例上执行（I-3S-1）。

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
    template: str = Form("harness_agent"),
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

    # --- normalise file & directory permissions -------------------------------
    # tarfile's "data" filter strips execute bits; zip doesn't preserve them.
    # Directories missing +x are inaccessible; scripts without +x fail at runtime.
    _normalize_permissions(pkg_dir)

    # --- §5.2 sync template public parts (_TEMPLATE_ONLY overlay) --------------
    # Re-apply template-owned infrastructure files (wrapper/*.sh, harness/tools/,
    # nodes/*.py entries, tests/) so framework upgrades propagate via update.
    # User-uploaded files matching _TEMPLATE_ONLY are clobbered (F5 single truth).
    tmpl_dir = settings.agent_template_path / template
    if tmpl_dir.is_dir():
        _reapply_template_files(tmpl_dir.resolve(), pkg_dir)
        _make_scripts_executable(pkg_dir)
    else:
        logger.warning("update.skip_template_sync: template dir not found: %s", tmpl_dir)

    # --- build PackageInfo ------------------------------------------------------
    file_count = 0
    try:
        file_count = sum(1 for p in pkg_dir.rglob("*") if p.is_file())
    except OSError:
        pass

    now = datetime.now(tz=UTC).isoformat(timespec="seconds")
    logger.info(
        "package_update.complete name=%s mode=%s files=%d archive=%s",
        safe_name, mode_lower, file_count, file.filename,
    )

    # --- hot-reload (only if already loaded in daemon) ---------------------------
    # _try_hot_reload_agent 内部已执行 setup.sh（在 ③ workspace 实例上）。
    await _try_hot_reload_agent(safe_name, client)

    # 三态模型：setup.sh 不在 ② 固化态执行（避免 .venv/ 污染），
    # 已由 _try_hot_reload_agent 在 ③ 上执行（I-3S-1）。

    return PackageInfo(
        name=safe_name,
        path=str(pkg_dir.resolve()),
        file_count=file_count,
        last_modified=now,
    )


# ---------------------------------------------------------------------------
# T5 CRUD surface
# ---------------------------------------------------------------------------


@router.get("", response_model=list[PackageInfo], summary="List agent packages on disk")
async def list_packages() -> list[PackageInfo]:
    """Return every directory under ``agent_packages_path`` as a package.

    Each entry mirrors the ``PackageInfo`` schema in :mod:`models`: name,
    absolute path, recursive file count, and ISO-8601 last-modified.

    Hidden directories (leading ``.``) and files are excluded so e.g.
    ``.git`` folders don't appear as fake packages.
    """
    return await _to_thread(_list_packages_sync)


@router.get(
    "/{name}/tree",
    response_model=list[FileNode],
    response_model_exclude_none=True,
    summary="Recursive file tree of a package",
)
async def get_package_tree(
    name: str = PathParam(..., description="Package directory name"),
) -> list[FileNode]:
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
