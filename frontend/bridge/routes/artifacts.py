"""Secure serving of generated chat artifacts from agent package directories.

The artifact router attempts to resolve file paths in two locations:
1. Inside ``agent_packages_path`` (e.g. ``examples/agent/``) — primary
2. Inside the project root — fallback, for files generated at the project root
   (e.g. ``.agentfoundry/outputs/``) where the ``relative_path`` includes the
   agent package directory prefix that needs to be stripped.
"""

import logging
from pathlib import Path

from fastapi import APIRouter, HTTPException, Query
from fastapi.responses import FileResponse

from config import settings

logger = logging.getLogger("agentflow.bridge.routes.artifacts")

router = APIRouter(prefix="/api/artifacts", tags=["artifacts"])
_IMAGE_SUFFIXES = {".png", ".jpg", ".jpeg", ".webp", ".gif"}
_DOCUMENT_SUFFIXES = {".pptx", ".ppt", ".docx", ".doc", ".xlsx", ".xls", ".pdf"}
_ARTIFACT_SUFFIXES = _IMAGE_SUFFIXES | _DOCUMENT_SUFFIXES | {".html", ".htm", ".css", ".js", ".json", ".svg"}


def _image_media_type(path: Path) -> str:
    try:
        header = path.read_bytes()[:12]
    except OSError:
        return "application/octet-stream"
    if header.startswith(b"\xff\xd8\xff"):
        return "image/jpeg"
    if header.startswith(b"\x89PNG\r\n\x1a\n"):
        return "image/png"
    if header.startswith((b"GIF87a", b"GIF89a")):
        return "image/gif"
    if header.startswith(b"RIFF") and header[8:12] == b"WEBP":
        return "image/webp"
    return "application/octet-stream"


def _resolve_artifact(relative_path: str, agent_pkg_name: str = "") -> Path:
    """Resolve a relative artifact path, searching multiple roots.

    Strategy:
    1. Primary: resolve inside ``agent_packages_path``.
       ``relative_path`` may already contain the agent package directory name
       (e.g. ``outfit_assistant/.agentfoundry/...``).
    2. Fallback: if the primary candidate does not exist AND the
       ``relative_path`` starts with the agent package directory name, strip
       that prefix and resolve the remainder inside the project root.

    Returns the resolved absolute Path, or ``None`` if no candidate exists.
    """
    root = settings.agent_packages_path.resolve()
    pkg_dir = Path(settings.agent_packages_dir)

    # ── Primary: inside agent_packages_path ────────────────────
    candidate = (root / relative_path).resolve()
    try:
        candidate.relative_to(root)
    except ValueError:
        candidate = None  # path traversal blocked

    # ── Fallback: project root ─────────────────────────────────
    # 仅当 agent_packages_dir 是相对路径时才启用回退。
    # 绝对路径意味着 packages 不在项目目录下，回退不适用。
    if (candidate is None or not candidate.is_file()) and not pkg_dir.is_absolute():
        parts = Path(relative_path).parts
        if parts and parts[0] == agent_pkg_name and len(parts) > 1:
            # Strip the agent package directory prefix
            remaining = "/".join(parts[1:])
            # Compute project root by going up from agent_packages_path
            # by the depth of agent_packages_dir
            depth = len(pkg_dir.parts)
            project_root = root.parents[depth - 1] if depth > 1 else root.parent
            fallback = (project_root / remaining).resolve()
            try:
                fallback.relative_to(project_root)
            except ValueError:
                pass  # traversal blocked
            else:
                if fallback.is_file():
                    return fallback

    return candidate


async def get_artifact(agent_id: str, path: str = Query(...)) -> FileResponse:
    """Return an image only when it resolves inside a allowed root."""
    _aid = agent_id  # capture before del for logging
    del agent_id  # Path containment is the authorization boundary for now.
    root = settings.agent_packages_path.resolve()
    candidate = Path(path)
    if not candidate.is_absolute():
        candidate = root / candidate
    candidate = candidate.resolve()

    try:
        candidate.relative_to(root)
    except ValueError as exc:
        logger.warning("artifact_serve.forbidden agent=%s path=%s (outside packages root)", _aid, path)
        raise HTTPException(status_code=403, detail="artifact path outside packages root") from exc
    if candidate.suffix.lower() not in _IMAGE_SUFFIXES:
        logger.warning("artifact_serve.unsupported agent=%s path=%s suffix=%s", _aid, path, candidate.suffix)
        raise HTTPException(status_code=415, detail="unsupported artifact type")
    if not candidate.is_file():
        logger.warning("artifact_serve.not_found agent=%s path=%s resolved=%s", _aid, path, candidate)
        raise HTTPException(status_code=404, detail="artifact not found")
    return FileResponse(candidate, media_type=_image_media_type(candidate))


@router.get("/{agent_id}/files/{relative_path:path}")
async def get_artifact_file(agent_id: str, relative_path: str) -> FileResponse:
    """Serve an artifact tree while preserving HTML-relative asset paths.

    Resolves ``relative_path`` inside the agent packages directory first,
    then falls back to the project root if the file is not found there
    (handles the case where tools generate files at the project root,
    e.g. ``.agentfoundry/outputs/``).
    """
    _aid = agent_id

    # Strip agent_id namespace prefix if present (e.g. "outfit-assistant/" → "")
    search_path = relative_path
    if search_path.startswith(agent_id + "/"):
        search_path = search_path[len(agent_id) + 1:]

    candidate = _resolve_artifact(search_path, agent_pkg_name=agent_id.replace("-", "_"))

    if candidate is None:
        logger.warning("artifact_serve.forbidden agent=%s path=%s (blocked traversal)", _aid, relative_path)
        raise HTTPException(status_code=403, detail="artifact path blocked")

    if candidate.suffix.lower() not in _ARTIFACT_SUFFIXES:
        logger.warning("artifact_serve.unsupported agent=%s path=%s suffix=%s", _aid, relative_path, candidate.suffix)
        raise HTTPException(status_code=415, detail="unsupported artifact type")

    if not candidate.is_file():
        logger.warning("artifact_serve.not_found agent=%s path=%s resolved=%s", _aid, relative_path, candidate)
        raise HTTPException(status_code=404, detail="artifact not found")

    media_type = _image_media_type(candidate) if candidate.suffix.lower() in _IMAGE_SUFFIXES else None
    return FileResponse(candidate, media_type=media_type)
