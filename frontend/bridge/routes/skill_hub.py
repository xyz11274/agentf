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
"""

from __future__ import annotations

import logging
import shutil
import tarfile
import tempfile
import zipfile
from datetime import datetime, timezone
from pathlib import Path
import re
from typing import Any

from fastapi import APIRouter, File, HTTPException, UploadFile, status
from pydantic import BaseModel, Field

from config import settings

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
    """Validate skill name (reuses package name validation rules).

    Only ``[a-zA-Z0-9_-]`` characters, 1-128 length, no ``..``, no ``/``.
    """
    if not name or len(name) > 128:
        raise _err(
            f"Invalid skill name: {name!r} (must be 1-128 characters)",
            status.HTTP_403_FORBIDDEN,
        )
    if not re.match(r"^[a-zA-Z0-9_-]+$", name):
        raise _err(
            f"Invalid skill name: {name!r} (only [a-zA-Z0-9_-] allowed)",
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


def _parse_frontmatter(text: str) -> dict[str, Any]:
    """Parse YAML-like frontmatter from SKILL.md content.

    Only extracts ``description``, ``tags``, and ``version`` fields.
    Returns defaults for any missing field.
    """
    result: dict[str, Any] = {
        "description": "",
        "tags": [],
        "version": "1.0.0",
    }
    if not text.startswith("---\n") and not text.startswith("---\r\n"):
        return result

    # Find closing ---
    end_idx = text.find("---", 3)
    if end_idx == -1:
        return result

    header = text[3:end_idx].strip()
    for line in header.splitlines():
        line = line.strip()
        if ":" not in line:
            continue
        key, _, val = line.partition(":")
        key = key.strip().lower()
        val = val.strip()

        if key == "description":
            result["description"] = val.strip('"').strip("'")
        elif key == "tags":
            # Support both YAML list [a, b] and comma-separated
            val_clean = val.strip('"').strip("'")
            if val_clean.startswith("[") and val_clean.endswith("]"):
                tags = [t.strip().strip('"').strip("'") for t in val_clean[1:-1].split(",")]
                result["tags"] = [t for t in tags if t]
            elif val_clean:
                result["tags"] = [t.strip() for t in val_clean.split(",") if t.strip()]
        elif key == "version":
            result["version"] = val.strip('"').strip("'") or "1.0.0"

    return result


def _build_skill_info(name: str, skill_path: Path) -> SkillInfo:
    """Build SkillInfo from a skill directory."""
    skill_md = skill_path / "SKILL.md"
    description = ""
    tags: list[str] = []
    version = "1.0.0"

    if skill_md.exists() and skill_md.is_file():
        try:
            text = skill_md.read_text(encoding="utf-8")
            fm = _parse_frontmatter(text)
            description = fm.get("description", "")
            tags = fm.get("tags", [])
            version = fm.get("version", "1.0.0")
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
    """List all skills in the hub (metadata only)."""
    base = settings.skill_hub_path
    if not base.exists() or not base.is_dir():
        return []

    skills: list[SkillInfo] = []
    try:
        for entry in sorted(base.iterdir()):
            if entry.is_dir() and not entry.name.startswith("."):
                info = _build_skill_info(entry.name, entry)
                skills.append(info)
    except OSError as exc:
        raise _err(f"Failed to read skill hub: {exc}", status.HTTP_500_INTERNAL_SERVER_ERROR) from exc

    return skills


@router.get("/{name}", response_model=SkillDetail, status_code=status.HTTP_200_OK)
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
            skill_content = skill_md.read_text(encoding="utf-8")
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


@router.delete("/{name}", status_code=status.HTTP_204_NO_CONTENT)
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


@router.post("/{name}/install", response_model=SkillInstallResponse, status_code=status.HTTP_200_OK)
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
        raise _err(
            f"Skill '{safe_name}' is already installed in package '{safe_target}'",
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
