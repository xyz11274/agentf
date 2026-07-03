"""Secure serving of generated chat artifacts from agent package directories."""

import logging
from pathlib import Path

from fastapi import APIRouter, HTTPException, Query
from fastapi.responses import FileResponse

from config import settings

logger = logging.getLogger("agentflow.bridge.routes.artifacts")

router = APIRouter(prefix="/api/artifacts", tags=["artifacts"])
_IMAGE_SUFFIXES = {".png", ".jpg", ".jpeg", ".webp", ".gif"}
_ARTIFACT_SUFFIXES = _IMAGE_SUFFIXES | {".html", ".htm", ".css", ".js", ".json", ".svg"}


def _image_media_type(path: Path) -> str:
    header = path.read_bytes()[:12]
    if header.startswith(b"\xff\xd8\xff"):
        return "image/jpeg"
    if header.startswith(b"\x89PNG\r\n\x1a\n"):
        return "image/png"
    if header.startswith((b"GIF87a", b"GIF89a")):
        return "image/gif"
    if header.startswith(b"RIFF") and header[8:12] == b"WEBP":
        return "image/webp"
    return "application/octet-stream"


async def get_artifact(agent_id: str, path: str = Query(...)) -> FileResponse:
    """Return an image only when it resolves inside the packages root."""
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
    """Serve an artifact tree while preserving HTML-relative asset paths."""
    _aid = agent_id  # capture before del for logging
    del agent_id
    root = settings.agent_packages_path.resolve()
    candidate = (root / relative_path).resolve()
    try:
        candidate.relative_to(root)
    except ValueError as exc:
        logger.warning("artifact_serve.forbidden agent=%s path=%s (outside packages root)", _aid, relative_path)
        raise HTTPException(status_code=403, detail="artifact path outside packages root") from exc
    if candidate.suffix.lower() not in _ARTIFACT_SUFFIXES:
        logger.warning("artifact_serve.unsupported agent=%s path=%s suffix=%s", _aid, relative_path, candidate.suffix)
        raise HTTPException(status_code=415, detail="unsupported artifact type")
    if not candidate.is_file():
        logger.warning("artifact_serve.not_found agent=%s path=%s resolved=%s", _aid, relative_path, candidate)
        raise HTTPException(status_code=404, detail="artifact not found")
    media_type = _image_media_type(candidate) if candidate.suffix.lower() in _IMAGE_SUFFIXES else None
    return FileResponse(candidate, media_type=media_type)
