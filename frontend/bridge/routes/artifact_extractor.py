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


# Patterns match any Markdown link/image whose URL points at the artifact API.
# 边界是 url 指向 ``/api/artifacts/`` 路径（agent 产物），不是 label 文本——
# LLM 自定义 label（如 ``![居家穿搭效果图](url)``）也是产物；用户随手
# ``![表情](https://emoji.example.com/x.png)`` 不是产物（url 不指向 artifact API）。
# 守 I-1：不误匹配用户正文里的随手图片引用。
_ARTIFACT_URL_INFIX = "/api/artifacts/"
# Markdown image: ![label](url) — label 任意，url 必须含 artifact API infix
_IMAGE_PATTERN = re.compile(r"!\[[^\]]*\]\(([^)]*" + re.escape(_ARTIFACT_URL_INFIX) + r"[^)]*)\)")
# Markdown link (non-image): [label](url) — 用 lookbehind 排除 ![...](...) 图片形式
_LINK_PATTERN = re.compile(r"(?<!!)\[[^\]]*\]\(([^)]*" + re.escape(_ARTIFACT_URL_INFIX) + r"[^)]*)\)")

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
    Type is inferred from the URL extension (image → html → download),
    not from the Markdown label — so LLM-custom labels like
    ``![居家穿搭效果图](url)`` are extracted as long as the URL points at
    the artifact API.
    """
    artifacts: list[ArtifactInfo] = []

    # 边界是 url 指向 ``/api/artifacts/`` 路径（agent 产物），label 任意。
    # 守 I-1：不误匹配用户正文里的随手图片引用（emoji、外部图床等）。
    seen_urls: set[str] = set()

    for pattern in (_IMAGE_PATTERN, _LINK_PATTERN):
        for match in pattern.finditer(content):
            url = match.group(1).strip()
            if url in seen_urls:
                continue
            seen_urls.add(url)

            # Type 一律从 url 后缀分流，不信任 label 文本或 `!` 标记
            art_type = _infer_artifact_type(url)

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
