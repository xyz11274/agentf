"""Content serializer — bridge-side multimodal content processing.

Phase B (2026-07-26):
- Accept content as ``str | list[ContentPart]``.
- Normalize string to ``[{type:text, text:str}]``.
- For each ``image_url`` part:
  1. Decode base64 → save to ``{agent_packages_dir}/{agent_id}/workspace/{user_id}_{client_id}/upload/{sha256}.{ext}``
  2. Replace with a ``text`` part describing the file path
- Return a JSON string of the flattened content array (only text parts).
- Downstream nodes always receive a text-only content array.

Design doc: ``docs/design/features/agent-package/agent-package-structure.md`` §3.6
Changelog:
  2026-07-26 Phase B: session-level isolation (user_id+client_id in path).
  2026-07-03 Phase A: initial implementation, shared ``workspace/upload/``.
"""

from __future__ import annotations

import base64
import hashlib
import json
import logging
import re
from pathlib import Path
from typing import Any

from config import settings
from models import ContentPart, ImagePart, TextPart

logger = logging.getLogger("agentflow.bridge.content_serializer")

_DATA_URL_RE = re.compile(r"^data:([a-zA-Z0-9.+-]+/([a-zA-Z0-9.+-]+));base64,(.+)$", re.DOTALL)
_MIME_TO_EXT = {"png": "png", "jpeg": "jpg", "jpg": "jpg", "webp": "webp", "gif": "gif"}


def _compute_upload_dir(agent_id: str, user_id: str, client_id: str) -> Path:
    """Return the per-session workspace upload directory.

    Path: ``{agent_packages_path}/{agent_id}/workspace/{user_id}_{client_id}/upload/``

    Raises ``ValueError`` if ``user_id`` or ``client_id`` is empty — session
    context is required for user-data isolation. No backward-compatible fallback.
    """
    if not user_id or not client_id:
        raise ValueError(
            "session-level upload path requires user_id and client_id, "
            f"got user_id={user_id!r} client_id={client_id!r}"
        )
    return settings.agent_packages_path / agent_id / "workspace" / f"{user_id}_{client_id}" / "upload"


def _land_image(data_url: str, upload_dir: Path) -> str | None:
    """Decode a ``data:`` URL, save to *upload_dir*, return absolute path.

    Returns ``None`` if the URL is not a valid ``data:`` image.
    Content-addressed (sha256[:16]) — duplicates overwrite, fine.
    """
    m = _DATA_URL_RE.match(data_url)
    if not m:
        return None
    _, subtype, b64_payload = m.group(1), m.group(2).lower(), m.group(3)
    try:
        decoded = base64.b64decode(b64_payload)
    except Exception:
        return None
    ext = _MIME_TO_EXT.get(subtype, "bin")
    sha = hashlib.sha256(decoded).hexdigest()[:16]
    upload_dir.mkdir(parents=True, exist_ok=True)
    target = upload_dir / f"{sha}.{ext}"
    if not target.exists():
        target.write_bytes(decoded)
        logger.info("landed image sha=%s size=%dKB → %s", sha, len(decoded) // 1024, target)
    return str(target.resolve())


def normalize_content(content: str | list[ContentPart] | list[dict[str, Any]]) -> list[dict[str, Any]]:
    """Normalize incoming content to a list of plain dicts.

    String → ``[{type:text, text:str}]`` (empty string → ``[]``).
    """
    if isinstance(content, str):
        return [{"type": "text", "text": content}] if content else []
    out: list[dict[str, Any]] = []
    for part in content:
        if isinstance(part, (TextPart, ImagePart)):
            out.append(part.model_dump())
        elif isinstance(part, dict) and "type" in part:
            out.append(part)
        else:
            logger.warning("content_serializer: skipping malformed part %r", part)
    return out


def flatten_to_text(parts: list[dict[str, Any]], upload_dir: Path, agent_root: Path | None = None) -> list[dict[str, Any]]:
    """Flatten a content array to text-only parts.

    ``image_url`` parts are land to disk and replaced with a ``text`` part
    describing the **sandbox-relative** path (``workspace/{session}/upload/{sha}.{ext}``).
    All other parts pass through unchanged.

    The relative path is computed from *agent_root* — the agent package root.
    Passing *agent_root* explicitly avoids the old 2-level parent traversal
    assumption (which breaks with session-level ``workspace/{user_id}_{client_id}/upload/``).
    When ``None``, falls back to ``upload_dir.parent.parent`` for backward compat.
    """
    upload_dir = upload_dir.resolve()
    if agent_root is None:
        agent_root = upload_dir.parent.parent  # backward compat: workspace/upload/ → 2 parents
    else:
        agent_root = Path(agent_root).resolve()
    result: list[dict[str, Any]] = []
    for part in parts:
        if part.get("type") != "image_url":
            result.append(part)
            continue
        url = (part.get("image_url") or {}).get("url", "")
        file_path = _land_image(url, upload_dir) if url.startswith("data:") else url
        if not file_path:
            result.append(part)
            continue
        # Convert host absolute path → sandbox-relative path.
        # F11: don't guess path semantics — _land_image returns upload_dir.resolve()/filename,
        # we strip agent_root to get workspace/upload/filename.
        try:
            rel = Path(file_path).resolve().relative_to(agent_root)
            shown_path = str(rel)
        except ValueError:
            # file_path is outside agent_root (e.g. URL passthrough) — show as-is
            shown_path = file_path
        result.append({
            "type": "text",
            "text": f"用户上传了图片，路径: {shown_path}",
        })
    return result


def prepare_content_for_daemon(
    content: str | list[ContentPart] | list[dict[str, Any]],
    agent_id: str = "",
    user_id: str = "",
    client_id: str = "",
) -> str:
    """Normalize → flatten images → return **plain text** (NOT JSON).

    This is the single entry point called by chat routes.
    Returns plain text because the daemon sets it as AF_INPUT — JSON
    strings in the content field cause the daemon's shell command
    construction to break (env var truncation).

    Text-only messages pass through unchanged (backward compatible).
    Images are landed to disk (``workspace/{user_id}_{client_id}/upload/``)
    and their paths are described inline (session-level isolation).
    """
    parts = normalize_content(content)
    if agent_id:
        upload_dir = _compute_upload_dir(agent_id, user_id, client_id)
        # agent_root = {agent_packages_path}/{agent_id}
        agent_root = settings.agent_packages_path / agent_id
        parts = flatten_to_text(parts, upload_dir, agent_root)
    # Return plain text (no JSON array) — daemon sets this as AF_INPUT
    # and coordinator reads it as the user message.
    texts = [p["text"] for p in parts if p.get("type") == "text"]
    return "\n".join(texts) if texts else content if isinstance(content, str) else ""
