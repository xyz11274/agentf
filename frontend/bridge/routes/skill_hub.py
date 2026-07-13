"""AgentFlow Bridge — Skill Hub REST routes.

Provides a central repository for SKILL.md skills under
``settings.skill_hub_path`` (default ``resource/skill_hub/``).

Endpoints
---------

``GET    /api/skill-hub``              — list all skills (metadata)
``GET    /api/skill-hub/{name}``        — skill detail (SKILL.md content)
``POST   /api/skill-hub/upload``        — upload a skill (single .md or archive)
``DELETE /api/skill-hub/{name}``        — delete a skill
``POST   /api/skill-hub/{name}/install``— install skill into an agent package
``GET    /api/skill-hub/{name}/tree``   — file tree of a skill
``GET    /api/skill-hub/{name}/file/**`` — read a file inside a skill
``PUT    /api/skill-hub/{name}/file/**`` — write a file inside a skill
``DELETE /api/skill-hub/{name}/file/**`` — delete a file inside a skill
``POST   /api/skill-hub/{name}/file/**/rename`` — rename a file
``POST   /api/skill-hub/{name}/file/**`` — create an empty file
"""

from __future__ import annotations

import asyncio
import logging
import mimetypes
import os
import shutil
import tarfile
import tempfile
import zipfile
from datetime import datetime, timezone
from pathlib import Path
import re
from typing import Any, List

from fastapi import APIRouter, File, HTTPException, UploadFile, status
from fastapi.responses import Response
from pydantic import BaseModel, Field

from config import settings
from models import FileNode
from routes.packages import (
    FileWriteRequest,
    FileRenameRequest,
    _validate_relative_path,
    _resolve_within,
    _to_thread,
)

logger = logging.getLogger("agentflow.bridge.routes.skill_hub")

router = APIRouter(prefix="/api/skill-hub", tags=["skill-hub"])

# ---------------------------------------------------------------------------
# Allowed upload suffixes
# ---------------------------------------------------------------------------
_ALLOWED_UPLOAD_SUFFIXES = (".md", ".zip", ".tar.gz", ".tgz")

# ---------------------------------------------------------------------------
# Data models
# ---------------------------------------------------------------------------


class SkillInfo(BaseModel):
    """Metadata for a skill in the central hub."""

    name: str
    display_name: str = ""
    description: str
    tags: list[str] = []
    version: str = "1.0.0"
    file_count: int = 0
    last_modified: str = ""


class SkillDetail(SkillInfo):
    """Full skill detail including SKILL.md content."""

    content: str = ""


class SkillInstallRequest(BaseModel):
    """Request body for installing a skill into an agent package."""

    target_package: str = Field(
        ...,
        min_length=1,
        max_length=128,
        description="Target agent package name.",
    )
    overwrite: bool = Field(
        False,
        description="If true, replace an already-installed skill.",
    )


class SkillInstallResponse(BaseModel):
    """Response after installing a skill."""

    name: str
    target_package: str
    target_path: str
    file_count: int


# ---------------------------------------------------------------------------
# Envelope helpers
# ---------------------------------------------------------------------------


def _err(detail: str, code: int = status.HTTP_400_BAD_REQUEST) -> HTTPException:
    return HTTPException(status_code=code, detail=detail)


# ---------------------------------------------------------------------------
# Validation helpers (reuse patterns from routes/packages.py)
# ---------------------------------------------------------------------------


def _validate_skill_name(name: str) -> str:
    """Validate a skill name, supporting nested paths via ``/``.

    Rules:
    - 1-128 characters
    - Each segment: ``[a-zA-Z0-9_-]``
    - Segments separated by single ``/``
    - No leading/trailing ``/``, no ``//``, no ``..`` (path traversal)
    - No empty segments
    """
    if not name or len(name) > 128:
        raise _err(
            f"Invalid skill name: {name!r} (must be 1-128 characters)",
            status.HTTP_403_FORBIDDEN,
        )
    if ".." in name:
        raise _err(
            f"Invalid skill name: {name!r} (path traversal '..' not allowed)",
            status.HTTP_403_FORBIDDEN,
        )
    if name.startswith("/") or name.endswith("/"):
        raise _err(
            f"Invalid skill name: {name!r} (cannot start or end with '/')",
            status.HTTP_403_FORBIDDEN,
        )
    if "//" in name:
        raise _err(
            f"Invalid skill name: {name!r} (empty segments not allowed)",
            status.HTTP_403_FORBIDDEN,
        )
    if not re.match(r"^[a-zA-Z0-9_/-]+$", name):
        raise _err(
            f"Invalid skill name: {name!r} (only [a-zA-Z0-9_-] and '/' separator allowed)",
            status.HTTP_403_FORBIDDEN,
        )
    return name


def _skill_dir(name: str) -> Path:
    """Return the absolute skill directory path, validating the name."""
    safe = _validate_skill_name(name)
    base = settings.skill_hub_path
    p = base / safe
    if not p.exists() or not p.is_dir():
        raise _err(f"Skill '{name}' not found", status.HTTP_404_NOT_FOUND)
    return p


# ---------------------------------------------------------------------------
# Frontmatter parsing (minimal)
# ---------------------------------------------------------------------------

# Regex to match a YAML list item like "- python" or "  - python"
_RE_LIST_ITEM = re.compile(r"^\s*-\s+(.+)$")


def _parse_tag_value(val: str) -> list[str]:
    """Parse a tag value from frontmatter into a list of strings.

    Supports:
    * Inline list: ``[python, logging]``
    * Comma-separated: ``python, logging``
    * Empty string (multi-line list follows)
    """
    val_clean = val.strip('"').strip("'")
    if val_clean.startswith("[") and val_clean.endswith("]"):
        tags = [t.strip().strip('"').strip("'") for t in val_clean[1:-1].split(",")]
        return [t for t in tags if t]
    if val_clean:
        return [t.strip() for t in val_clean.split(",") if t.strip()]
    return []


def _parse_frontmatter(text: str) -> dict[str, Any]:
    """Parse YAML-like frontmatter from SKILL.md content.

    Extracts ``name``, ``description``, ``tags``, and ``version`` fields.
    Supports inline syntax (``key: value``) and multi-line YAML lists.

    * Inline list: ``tags: [python, logging]``
    * Comma list:  ``tags: python, logging``
    * Multi-line::

        tags:
          - python
          - logging

    Returns defaults for any missing field.
    """
    result: dict[str, Any] = {
        "description": "",
        "tags": [],
        "version": "1.0.0",
        "name": "",
    }
    if not text.startswith("---\n") and not text.startswith("---\r\n"):
        return result

    # Find closing ---
    end_idx = text.find("---", 3)
    if end_idx == -1:
        return result

    header = text[3:end_idx]
    lines = header.splitlines()

    # Track multi-line list context
    list_key: str | None = None

    for raw_line in lines:
        stripped = raw_line.strip()

        # ── Multi-line YAML list item ──────────────────────────────
        # A line starting with "- " (possibly indented) is a list item
        # for the currently tracked list_key.
        list_match = _RE_LIST_ITEM.match(stripped)
        if list_match and list_key is not None:
            item = list_match.group(1).strip().strip('"').strip("'")
            if item:
                result[list_key].append(item)  # type: ignore[attr-defined]
            continue

        # Reset list context when we hit a non-list-item line
        list_key = None

        if ":" not in stripped:
            continue
        key, _, val = stripped.partition(":")
        key = key.strip().lower()
        val = val.strip()

        if key == "description":
            result["description"] = val.strip('"').strip("'")
        elif key == "tags":
            collected = _parse_tag_value(val)
            result["tags"] = collected
            # If val is empty (multi-line list upcoming), enter list context
            if not collected and not val:
                list_key = "tags"
                result["tags"] = []
        elif key == "version":
            result["version"] = val.strip('"').strip("'") or "1.0.0"
        elif key == "name":
            result["name"] = val.strip('"').strip("'")

    return result


def _strip_frontmatter(text: str) -> str:
    """Strip YAML frontmatter (``---\\n...\\n---``) from the beginning of text.

    Returns the content after the closing ``---``, or the original text
    if no frontmatter is found.
    """
    if not text.startswith("---\n") and not text.startswith("---\r\n"):
        return text
    end_idx = text.find("---", 3)
    if end_idx == -1:
        return text
    # Skip the closing --- and the following newline
    rest = text[end_idx + 3:]
    if rest.startswith("\n"):
        rest = rest[1:]
    elif rest.startswith("\r\n"):
        rest = rest[2:]
    return rest


def _build_skill_info(name: str, skill_path: Path) -> SkillInfo:
    """Build SkillInfo from a skill directory."""
    skill_md = skill_path / "SKILL.md"
    description = ""
    tags: list[str] = []
    version = "1.0.0"
    display_name = ""

    if skill_md.exists() and skill_md.is_file():
        try:
            text = skill_md.read_text(encoding="utf-8")
            fm = _parse_frontmatter(text)
            description = fm.get("description", "")
            tags = fm.get("tags", [])
            version = fm.get("version", "1.0.0")
            display_name = fm.get("name", "")
        except OSError:
            pass

    file_count = 0
    last_modified = ""
    try:
        file_count = sum(1 for p in skill_path.rglob("*") if p.is_file())
        mtime = skill_md.stat().st_mtime if skill_md.exists() else skill_path.stat().st_mtime
        last_modified = datetime.fromtimestamp(mtime, tz=timezone.utc).isoformat(timespec="seconds")
    except OSError:
        pass

    return SkillInfo(
        name=name,
        display_name=display_name,
        description=description,
        tags=tags,
        version=version,
        file_count=file_count,
        last_modified=last_modified,
    )


def _infer_skill_name(filename: str) -> str:
    """Infer skill name from filename by stripping known extensions."""
    lower = filename.lower()
    if lower.endswith(".tar.gz"):
        stem = filename[:-7]
    elif lower.endswith(".tgz"):
        stem = filename[:-4]
    elif lower.endswith(".zip"):
        stem = filename[:-4]
    elif lower.endswith(".md"):
        stem = filename[:-3]
    else:
        stem = filename
    # Remove leading/trailing dots and spaces
    return stem.strip(". ")


# ---------------------------------------------------------------------------
# Endpoints
# ---------------------------------------------------------------------------


@router.get("", response_model=list[SkillInfo], status_code=status.HTTP_200_OK)
async def list_skills() -> list[SkillInfo]:
    """List all skills in the hub (metadata only).

    Discovers skills by recursively searching for directories containing
    a ``SKILL.md`` file.  Nested paths (e.g. ``dashscope/forge-image-gen``)
    are used as the skill name, preserving the directory hierarchy.
    """
    base = settings.skill_hub_path
    if not base.exists() or not base.is_dir():
        return []

    skills: list[SkillInfo] = []
    try:
        for skill_md in sorted(base.rglob("SKILL.md")):
            dir_path = skill_md.parent
            # Skip hidden directories
            if any(part.startswith(".") for part in dir_path.relative_to(base).parts):
                continue
            # Use the relative path as the skill name (e.g. "dashscope/forge-image-gen")
            name = str(dir_path.relative_to(base))
            info = _build_skill_info(name, dir_path)
            skills.append(info)
    except OSError as exc:
        raise _err(f"Failed to read skill hub: {exc}", status.HTTP_500_INTERNAL_SERVER_ERROR) from exc

    return skills


# ---------------------------------------------------------------------------
# File tree + file CRUD
# ---------------------------------------------------------------------------


def _skill_tree_path(name: str) -> Path:
    """Validate skill name and return the skill directory path."""
    safe = _validate_skill_name(name)
    skill_dir = settings.skill_hub_path / safe
    if not skill_dir.exists() or not skill_dir.is_dir():
        raise _err(f"Skill '{name}' not found", status.HTTP_404_NOT_FOUND)
    return skill_dir


def _build_skill_tree_sync(skill_dir: Path) -> List[FileNode]:
    """Recursively build a sorted file tree rooted at ``skill_dir``.

    Skips runtime / cache directories (``__pycache__``, ``node_modules``,
    ``.venv``, ``venv``, ``.git``, ``__pycache__/``, ``*.pyc`` files).
    """

    _EXCLUDE_DIRS = frozenset({
        "__pycache__", "node_modules", ".venv", "venv",
        ".git", ".svn", ".idea", ".vscode",
    })

    def _node(p: Path) -> FileNode:
        rel = p.relative_to(skill_dir)
        if p.is_dir():
            children = sorted(
                (c for c in p.iterdir() if c.name not in _EXCLUDE_DIRS),
                key=lambda c: (not c.is_dir(), c.name),
            )
            return FileNode(
                name=p.name,
                path=str(rel),
                type="dir",
                children=[_node(c) for c in children],
            )
        # Skip .pyc files
        if p.suffix == ".pyc":
            return None
        return FileNode(name=p.name, path=str(rel), type="file")

    children = sorted(
        (c for c in skill_dir.iterdir() if c.name not in _EXCLUDE_DIRS),
        key=lambda c: (not c.is_dir(), c.name),
    )
    return [_node(c) for c in children if _node(c) is not None]


@router.get(
    "/{name:path}/tree",
    response_model=List[FileNode],
    response_model_exclude_none=True,
    summary="Recursive file tree of a skill",
)
async def get_skill_tree(
    name: str,
) -> List[FileNode]:
    """Return the recursive file tree of a single skill."""
    skill_dir = _skill_tree_path(name)
    return await _to_thread(_build_skill_tree_sync, skill_dir)


@router.get(
    "/{name:path}/file/{file_path:path}",
    summary="Read a file inside a skill",
)
async def read_skill_file(
    name: str,
    file_path: str,
) -> dict[str, str]:
    """Read the UTF-8 contents of a file inside the skill."""
    skill_dir = _skill_tree_path(name)
    segments = _validate_relative_path(file_path)
    target = _resolve_within(skill_dir, segments)
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
    "/{name:path}/file/{file_path:path}",
    status_code=status.HTTP_200_OK,
    summary="Write file content (create or overwrite)",
)
async def write_skill_file(
    body: FileWriteRequest,
    name: str,
    file_path: str,
) -> dict[str, Any]:
    """Create or overwrite a file with the supplied UTF-8 content."""
    skill_dir = _skill_tree_path(name)
    segments = _validate_relative_path(file_path)
    target = _resolve_within(skill_dir, segments)
    target.parent.mkdir(parents=True, exist_ok=True)
    await _to_thread(lambda: target.write_text(body.content, encoding="utf-8"))
    logger.info("Wrote %d bytes to %s/%s", len(body.content), name, file_path)
    return {"path": file_path, "bytes": len(body.content.encode("utf-8"))}


@router.delete(
    "/{name:path}/file/{file_path:path}",
    status_code=status.HTTP_204_NO_CONTENT,
    summary="Delete a file inside a skill",
)
async def delete_skill_file(
    name: str,
    file_path: str,
) -> None:
    """Delete a file inside the skill."""
    skill_dir = _skill_tree_path(name)
    segments = _validate_relative_path(file_path)
    target = _resolve_within(skill_dir, segments)
    if not target.exists():
        raise HTTPException(
            status_code=status.HTTP_404_NOT_FOUND, detail="File not found"
        )
    if target.is_dir():
        raise HTTPException(
            status_code=status.HTTP_409_CONFLICT,
            detail="Use the directory operation to delete a directory",
        )
    await _to_thread(target.unlink)
    logger.info("Deleted file %s/%s", name, file_path)


@router.post(
    "/{name:path}/file/{file_path:path}/rename",
    status_code=status.HTTP_200_OK,
    summary="Rename a file within its parent directory",
)
async def rename_skill_file(
    body: FileRenameRequest,
    name: str,
    file_path: str,
) -> dict[str, str]:
    """Rename a file within its parent directory."""
    skill_dir = _skill_tree_path(name)
    segments = _validate_relative_path(file_path)
    target = _resolve_within(skill_dir, segments)
    if not target.exists():
        raise HTTPException(
            status_code=status.HTTP_404_NOT_FOUND, detail="File not found"
        )
    if target.is_dir():
        raise HTTPException(
            status_code=status.HTTP_409_CONFLICT,
            detail="Use a directory operation to rename directories",
        )
    if not body.new_name or "/" in body.new_name or "\\" in body.new_name:
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST,
            detail="Invalid new_name: must be a single file name without path separators",
        )
    new_path = target.with_name(body.new_name)
    if new_path.exists():
        raise HTTPException(
            status_code=status.HTTP_409_CONFLICT,
            detail=f"Target file '{body.new_name}' already exists",
        )
    await _to_thread(target.rename, new_path)
    logger.info("Renamed %s/%s → %s", name, file_path, body.new_name)
    return {"old_path": file_path, "new_path": str(new_path.relative_to(skill_dir))}


@router.post(
    "/{name:path}/file/{file_path:path}",
    status_code=status.HTTP_201_CREATED,
    summary="Create a new empty file",
)
async def create_skill_file(
    name: str,
    file_path: str,
) -> dict[str, str]:
    """Create a new empty file inside the skill."""
    skill_dir = _skill_tree_path(name)
    segments = _validate_relative_path(file_path)
    target = _resolve_within(skill_dir, segments)
    if target.exists():
        raise HTTPException(
            status_code=status.HTTP_409_CONFLICT, detail="File already exists"
        )
    target.parent.mkdir(parents=True, exist_ok=True)
    await _to_thread(lambda: target.write_text("", encoding="utf-8"))
    logger.info("Created empty file %s/%s", name, file_path)
    return {"path": file_path}


@router.get(
    "/{name:path}/media/{media_path:path}",
    summary="Serve raw file (images, etc.) for inline display",
)
async def serve_skill_media(
    name: str,
    media_path: str,
) -> Any:
    """Serve a file inside the skill as raw bytes with correct Content-Type.

    Used to display images and other media inline in the browser (e.g.
    Markdown image references, file tree previews).
    """
    skill_dir = _skill_tree_path(name)
    segments = _validate_relative_path(media_path)
    target = _resolve_within(skill_dir, segments)
    if not target.exists() or not target.is_file():
        raise HTTPException(
            status_code=status.HTTP_404_NOT_FOUND, detail="File not found"
        )
    content_type, _ = mimetypes.guess_type(str(target))
    if content_type is None:
        content_type = "application/octet-stream"
    body = await _to_thread(target.read_bytes)
    return Response(content=body, media_type=content_type)


@router.get("/{name:path}", response_model=SkillDetail, status_code=status.HTTP_200_OK)
async def get_skill(name: str) -> SkillDetail:
    """Get skill detail including SKILL.md full content."""
    safe = _validate_skill_name(name)
    base = settings.skill_hub_path
    skill_path = base / safe

    if not skill_path.exists() or not skill_path.is_dir():
        raise _err(f"Skill '{name}' not found", status.HTTP_404_NOT_FOUND)

    info = _build_skill_info(name, skill_path)

    skill_content = ""
    skill_md = skill_path / "SKILL.md"
    if skill_md.exists() and skill_md.is_file():
        try:
            raw = skill_md.read_text(encoding="utf-8")
            skill_content = _strip_frontmatter(raw)
        except OSError as exc:
            raise _err(
                f"Failed to read SKILL.md: {exc}", status.HTTP_500_INTERNAL_SERVER_ERROR
            ) from exc

    return SkillDetail(**info.model_dump(), content=skill_content)


@router.post("/upload", response_model=SkillInfo, status_code=status.HTTP_201_CREATED)
async def upload_skill(
    file: UploadFile = File(..., description="SKILL.md or archive (.zip/.tar.gz/.tgz)."),
    name: str | None = None,
    overwrite: bool = False,
) -> SkillInfo:
    """Upload a skill (single SKILL.md or archive) to the hub.

    ``file`` must be provided as ``multipart/form-data``.
    When ``name`` is omitted it is inferred from the filename.
    Set ``overwrite=true`` to replace an existing skill.
    """
    # --- validate file extension --------------------------------------------
    if not file.filename:
        raise _err("Upload filename is required", status.HTTP_400_BAD_REQUEST)

    lower_name = file.filename.lower()
    if not lower_name.endswith(_ALLOWED_UPLOAD_SUFFIXES):
        raise _err(
            f"Unsupported file format: {file.filename!r}. "
            f"Allowed: {', '.join(_ALLOWED_UPLOAD_SUFFIXES)}",
            status.HTTP_400_BAD_REQUEST,
        )

    is_single_md = lower_name.endswith(".md")
    is_tar = lower_name.endswith((".tar.gz", ".tgz"))
    is_zip = lower_name.endswith(".zip")

    # --- determine skill name -----------------------------------------------
    if name:
        _validate_skill_name(name)
        if "/" in name:
            raise _err(
                "Uploaded skill name must be flat (no '/'): nested paths are "
                "for pre-existing directory structures only.",
                status.HTTP_400_BAD_REQUEST,
            )
    else:
        name = _infer_skill_name(file.filename)
        _validate_skill_name(name)

    safe_name = name
    base = settings.skill_hub_path
    skill_dir = base / safe_name

    if skill_dir.exists() and not overwrite:
        raise _err(
            f"Skill '{safe_name}' already exists. Set overwrite=true to replace.",
            status.HTTP_409_CONFLICT,
        )

    # --- save uploaded file to temp location --------------------------------
    try:
        suffix = ".tar.gz" if is_tar else ".zip" if is_zip else ".md"
        with tempfile.NamedTemporaryFile(delete=False, suffix=suffix) as tmp:
            tmp_path = Path(tmp.name)
            file_content = await file.read()
            tmp.write(file_content)
    except OSError as exc:
        raise _err(
            f"Failed to save uploaded file: {exc}", status.HTTP_500_INTERNAL_SERVER_ERROR
        ) from exc

    # --- create or replace skill directory ----------------------------------
    try:
        if skill_dir.exists():
            shutil.rmtree(skill_dir)

        if is_single_md:
            skill_dir.mkdir(parents=True, exist_ok=True)
            (skill_dir / "SKILL.md").write_bytes(file_content)
        elif is_zip:
            with zipfile.ZipFile(tmp_path, "r") as zf:
                zf.extractall(path=skill_dir)
        elif is_tar:
            with tarfile.open(tmp_path, "r:gz") as tf:
                tf.extractall(path=skill_dir, filter="data")

        # Validate: SKILL.md must exist at root
        if not (skill_dir / "SKILL.md").exists():
            # Clean up partial directory
            shutil.rmtree(skill_dir, ignore_errors=True)
            raise _err(
                "Archive does not contain SKILL.md at its root",
                status.HTTP_400_BAD_REQUEST,
            )

    except (tarfile.TarError, zipfile.BadZipFile, OSError) as exc:
        if skill_dir.exists():
            shutil.rmtree(skill_dir, ignore_errors=True)
        raise _err(
            f"Failed to process upload: {exc}",
            status.HTTP_400_BAD_REQUEST,
        ) from exc
    finally:
        tmp_path.unlink(missing_ok=True)

    info = _build_skill_info(safe_name, skill_dir)
    logger.info("Uploaded skill '%s' (%d files)", safe_name, info.file_count)
    return info


@router.delete("/{name:path}", status_code=status.HTTP_204_NO_CONTENT)
async def delete_skill(name: str) -> None:
    """Delete a skill from the hub."""
    skill_path = _skill_dir(name)
    try:
        shutil.rmtree(skill_path)
    except OSError as exc:
        raise _err(
            f"Failed to delete skill '{name}': {exc}",
            status.HTTP_500_INTERNAL_SERVER_ERROR,
        ) from exc
    logger.info("Deleted skill '%s'", name)


@router.post("/{name:path}/install", response_model=SkillInstallResponse, status_code=status.HTTP_200_OK)
async def install_skill(
    name: str,
    body: SkillInstallRequest,
) -> SkillInstallResponse:
    """Install a skill from the hub into an agent package.

    The skill is copied from ``skill_hub/{name}`` to
    ``{agent_packages}/{target_package}/harness/skills/{name}``.
    """
    safe_name = _validate_skill_name(name)
    safe_target = _validate_skill_name(body.target_package)
    if "/" in safe_target:
        raise _err(
            f"Invalid target package name: {safe_target!r} (flat name only, no '/' allowed)",
            status.HTTP_400_BAD_REQUEST,
        )

    # Source: skill hub
    src = settings.skill_hub_path / safe_name
    if not src.exists() or not src.is_dir():
        raise _err(f"Skill '{name}' not found in hub", status.HTTP_404_NOT_FOUND)

    # Target: agent package
    pkg_path = settings.agent_packages_path / safe_target
    if not pkg_path.exists() or not pkg_path.is_dir():
        raise _err(
            f"Target package '{safe_target}' not found",
            status.HTTP_404_NOT_FOUND,
        )

    dest = pkg_path / "harness" / "skills" / safe_name
    if dest.exists():
        if body.overwrite:
            try:
                shutil.rmtree(dest)
            except OSError as exc:
                raise _err(
                    f"Failed to remove existing skill for overwrite: {exc}",
                    status.HTTP_500_INTERNAL_SERVER_ERROR,
                ) from exc
        else:
            raise _err(
                f"Skill '{safe_name}' is already installed in package '{safe_target}'. "
                "Set overwrite=true to replace.",
                status.HTTP_409_CONFLICT,
            )

    # Ensure parent directory exists
    try:
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copytree(src, dest)
    except OSError as exc:
        # Clean up partial copy
        if dest.exists():
            shutil.rmtree(dest, ignore_errors=True)
        raise _err(
            f"Failed to install skill: {exc}",
            status.HTTP_500_INTERNAL_SERVER_ERROR,
        ) from exc

    file_count = 0
    try:
        file_count = sum(1 for p in dest.rglob("*") if p.is_file())
    except OSError:
        pass

    logger.info(
        "Installed skill '%s' → package '%s' (%d files)",
        safe_name, safe_target, file_count,
    )
    return SkillInstallResponse(
        name=safe_name,
        target_package=safe_target,
        target_path=str(dest.resolve()),
        file_count=file_count,
    )


__all__ = ["router"]
