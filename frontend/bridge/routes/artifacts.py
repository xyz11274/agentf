"""Secure serving of generated chat artifacts from agent package directories."""

from pathlib import Path

from fastapi import APIRouter, HTTPException, Query
from fastapi.responses import FileResponse

from config import settings


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


@router.get("/{agent_id}")
async def get_artifact(agent_id: str, path: str = Query(...)) -> FileResponse:
    """Return an image only when it resolves inside the packages root."""
    del agent_id  # Path containment is the authorization boundary for now.
    root = settings.agent_packages_path.resolve()
    candidate = Path(path)
    if not candidate.is_absolute():
        candidate = root / candidate
    candidate = candidate.resolve()

    try:
        candidate.relative_to(root)
    except ValueError as exc:
        raise HTTPException(status_code=403, detail="artifact path outside packages root") from exc
    if candidate.suffix.lower() not in _IMAGE_SUFFIXES:
        raise HTTPException(status_code=415, detail="unsupported artifact type")
    if not candidate.is_file():
        raise HTTPException(status_code=404, detail="artifact not found")
    return FileResponse(candidate, media_type=_image_media_type(candidate))


@router.get("/{agent_id}/files/{relative_path:path}")
async def get_artifact_file(agent_id: str, relative_path: str) -> FileResponse:
    """Serve an artifact tree while preserving HTML-relative asset paths."""
    del agent_id
    root = settings.agent_packages_path.resolve()
    candidate = (root / relative_path).resolve()
    try:
        candidate.relative_to(root)
    except ValueError as exc:
        raise HTTPException(status_code=403, detail="artifact path outside packages root") from exc
    if candidate.suffix.lower() not in _ARTIFACT_SUFFIXES:
        raise HTTPException(status_code=415, detail="unsupported artifact type")
    if not candidate.is_file():
        raise HTTPException(status_code=404, detail="artifact not found")
    media_type = _image_media_type(candidate) if candidate.suffix.lower() in _IMAGE_SUFFIXES else None
    return FileResponse(candidate, media_type=media_type)
