"""Shared artifact extraction logic for chat response processing.

Provides:
- ``extract_artifact_infos(content)`` — parses ``ChatChunk.content`` for
  artifact Markdown patterns and returns structured ``ArtifactInfo`` objects.
- ``serialize_artifacts(content)`` — convenience wrapper that extracts and
  JSON-serializes artifacts for database persistence.
- ``publicize_artifact_urls(content)`` — rewrites relative ``/api/artifacts/``
  URLs to public absolute URLs when ``artifact_public_base_url`` is configured.

This is consumed by both ``routes/chat.py`` (WebSocket) and
``routes/chat_rest.py`` (REST SSE) to populate the ``ChatChunk.artifacts`` field.
"""

from __future__ import annotations

import json
import mimetypes
import re
from pathlib import Path

from config import settings
from models import ArtifactInfo


# Patterns match the output of coordinator's ``append_artifacts_markdown``.
# Each captures the URL and infers the artifact type from the markdown format.
_IMAGE_PATTERN = re.compile(r"!\[生成图片 \d+\]\(([^)]+)\)")
_HTML_PATTERN = re.compile(r"\[查看报告 \d+\]\(([^)]+)\)")
_DOWNLOAD_PATTERN = re.compile(r"\[📥 下载文件 \d+\]\(([^)]+)\)")

# URL-based type inference for legacy artifacts without structured markers
_IMAGE_EXTENSIONS = {".png", ".jpg", ".jpeg", ".webp", ".gif"}
_HTML_EXTENSIONS = {".html", ".htm"}
_DOWNLOAD_EXTENSIONS = {".pptx", ".ppt", ".docx", ".doc", ".xlsx", ".xls", ".pdf", ".zip"}


def _infer_artifact_type(url: str) -> str:
    """Infer artifact type from URL extension."""
    lower = url.lower()
    ext = Path(lower.split("?")[0]).suffix
    if ext in _IMAGE_EXTENSIONS:
        return "image"
    if ext in _HTML_EXTENSIONS:
        return "html"
    if ext in _DOWNLOAD_EXTENSIONS:
        return "download"
    # Default: try mimetypes
    guessed, _ = mimetypes.guess_type(url)
    if guessed:
        if guessed.startswith("image/"):
            return "image"
        if guessed == "text/html":
            return "html"
    return "download"


def _url_filename(url: str) -> str:
    """Extract filename from a URL path."""
    path = url.split("?")[0].rstrip("/")
    name = path.split("/")[-1]
    return name if name else "artifact"


def _infer_mime_type(url: str, art_type: str) -> str:
    """Guess MIME type from URL extension, falling back to type-based default."""
    guessed, _ = mimetypes.guess_type(url.split("?")[0])
    if guessed:
        return guessed
    return {
        "image": "image/png",
        "html": "text/html",
        "download": "application/octet-stream",
    }.get(art_type, "application/octet-stream")


def extract_artifact_infos(content: str) -> list[ArtifactInfo]:
    """Parse ChatChunk content for artifact Markdown patterns.

    Returns structured ``ArtifactInfo`` objects in the order they appear.
    Uses three marker patterns in order (image → html → download).
    Falls back to URL extension for type inference when marker is ambiguous.
    """
    artifacts: list[ArtifactInfo] = []

    # Pattern-based extraction (matches append_artifacts_markdown output)
    seen_urls: set[str] = set()

    for pattern, marker_type in [
        (_IMAGE_PATTERN, "image"),
        (_HTML_PATTERN, "html"),
        (_DOWNLOAD_PATTERN, "download"),
    ]:
        for match in pattern.finditer(content):
            url = match.group(1).strip()
            if url in seen_urls:
                continue
            seen_urls.add(url)
            url_lower = url.lower()

            # For image/html patterns, trust the marker; for download,
            # double-check the extension isn't actually an image
            art_type = marker_type
            if marker_type == "download":
                ext = Path(url_lower.split("?")[0]).suffix
                if ext in _IMAGE_EXTENSIONS:
                    art_type = "image"
                elif ext in _HTML_EXTENSIONS:
                    art_type = "html"

            artifacts.append(ArtifactInfo(
                type=art_type,  # type: ignore[arg-type]
                url=url,
                name=_url_filename(url),
                mime_type=_infer_mime_type(url, art_type),
            ))

    return artifacts


def serialize_artifacts(content: str) -> str:
    """Extract structured artifacts from response content and serialize to JSON.

    Returns a JSON string suitable for the ``artifacts`` column in
    ``save_dialogue()``.
    """
    artifacts = extract_artifact_infos(content)
    if not artifacts:
        return "[]"
    return json.dumps([a.model_dump() for a in artifacts], ensure_ascii=False)


def publicize_artifact_urls(content: str) -> str:
    """Rewrite relative artifact URLs to public absolute URLs.

    When ``artifact_public_base_url`` is configured (e.g.
    ``https://cdn.example.com``), rewrites ``(/api/artifacts/...`` to
    ``(https://cdn.example.com/api/artifacts/...``.
    """
    base = settings.artifact_public_base_url.rstrip("/")
    if not base:
        return content
    return content.replace("(/api/artifacts/", f"({base}/api/artifacts/")
