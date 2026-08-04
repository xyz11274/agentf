"""Secure serving of generated chat artifacts from file storage service.

Phase 2: Bridge 自身作为存储中心（见 docs/design/features/sandbox/artifact-storage.md §3.3）。

- 上传：POST /api/artifacts/upload → 路径安全校验 → 写入本地文件系统
- 下载：GET /api/artifacts/{artifact_id} → 从本地文件系统读取
- 不再转发到外部存储服务（沙箱 HTTP server 不再参与产物存储链路）
"""

import logging
import re
from pathlib import Path

import base64
from fastapi import APIRouter, HTTPException
from fastapi.responses import FileResponse

from config import settings

logger = logging.getLogger("agentflow.bridge.routes.artifacts")

router = APIRouter(prefix="/api/artifacts", tags=["artifacts"])
_IMAGE_SUFFIXES = {".png", ".jpg", ".jpeg", ".webp", ".gif"}
_DOCUMENT_SUFFIXES = {".pptx", ".ppt", ".docx", ".doc", ".xlsx", ".xls", ".pdf"}
_ARTIFACT_SUFFIXES = _IMAGE_SUFFIXES | _DOCUMENT_SUFFIXES | {".html", ".htm", ".css", ".js", ".json", ".svg"}

# ── 产物存储根目录 ──────────────────────────────────────────────────────
# 默认与 agent_packages_path 同级，可由环境变量 ARTIFACTS_STORAGE_DIR 覆盖
_ARTIFACTS_STORAGE_DIR = Path(
    getattr(settings, "artifacts_storage_dir", "") or
    str(settings.agent_packages_path.parent / "artifacts_storage")
)


# ── 路径白名单（I-upload-traversal-block 第一、二层）────────────────────
# 段字符集：字母/数字/._- （含 _，与 agentflow session_id "{user}_{client}" 格式兼容）
# 注意：首字符集 [A-Za-z0-9._] 不含 -，后续字符集 [A-Za-z0-9._-] 含 -。
# session_id 格式为 {user_id}_{client_id}，不含 -，当前无实际风险。
_ARTIFACT_PATH_SEGMENT_RE = re.compile(r"^[A-Za-z0-9._][A-Za-z0-9._-]*$")


def _is_safe_path_segment(segment: str) -> bool:
    """单段路径白名单（agent_name / session_id）：拒含 `/` 或 `..` 或空。"""
    if not segment:
        return False
    return bool(_ARTIFACT_PATH_SEGMENT_RE.match(segment)) and segment != "." and segment != ".."


def _is_safe_relative_path(path: str) -> bool:
    """相对路径白名单：拒绝对对路径、`..` 段、控制字符。

    控制字符（< 0x20）在部分文件系统上可能引发异常或被视为特殊字符，
    此处显式阻断避免歧义（M3 可定位错误信息）。
    """
    if not path:
        return False
    if path.startswith("/") or path.startswith("\\"):
        return False
    # 控制字符检查（< 0x20 = 空格前的所有 ASCII 控制字符）
    if any(ord(c) < 0x20 for c in path):
        return False
    parts = path.split("/")
    for part in parts:
        if not _is_safe_path_segment(part):
            return False
    return True


def _image_media_type(path: Path) -> str:
    """读取文件头 12 字节推断图片 MIME 类型，避免读取整个文件。"""
    try:
        with path.open("rb") as f:
            header = f.read(12)
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
    2. Fallback: strip session_id segment and try again.
    3. Fallback: project root (legacy paths).
    """
    root = settings.agent_packages_path.resolve()
    pkg_dir = Path(settings.agent_packages_dir)

    candidate = (root / relative_path).resolve()
    try:
        candidate.relative_to(root)
    except ValueError:
        logger.warning("artifact_resolve.traversal_blocked path=%s root=%s", relative_path, root)
        candidate = None

    if candidate is None or not candidate.is_file():
        # Fallback: strip session_id segment (second segment) and retry
        parts = Path(relative_path).parts
        if len(parts) >= 3 and parts[0] == agent_pkg_name:
            # Try without session_id: agent-sandbox/1/outputs/file.png → agent-sandbox/outputs/file.png
            stripped = "/".join([parts[0]] + list(parts[2:]))
            alt = (root / stripped).resolve()
            try:
                alt.relative_to(root)
            except ValueError:
                pass
            else:
                if alt.is_file():
                    return alt

    if (candidate is None or not candidate.is_file()) and not pkg_dir.is_absolute():
        parts = Path(relative_path).parts
        if parts and parts[0] == agent_pkg_name and len(parts) > 1:
            remaining = "/".join(parts[1:])
            depth = len(pkg_dir.parts)
            project_root = root.parents[depth - 1] if depth > 1 else root.parent
            fallback = (project_root / remaining).resolve()
            try:
                fallback.relative_to(project_root)
            except ValueError:
                pass
            else:
                if fallback.is_file():
                    return fallback

    return candidate


# ── 上传端点 ────────────────────────────────────────────────────────────


@router.post("/upload")
async def upload_artifact(body: dict) -> dict:
    """上传产物到 bridge 存储中心。

    请求体格式（与设计文档 §2.3 接口契约一致）：
    ```json
    {
      "agent_name": "outfit-assistant",
      "session_id": "user_abc_client_123",
      "files": [
        {"path": "outputs/look.png", "data_b64": "<base64>"}
      ]
    }
    ```
    """
    agent_name = body.get("agent_name", "")
    session_id = body.get("session_id", "")
    files = body.get("files", [])

    if not agent_name or not session_id or not files:
        raise HTTPException(status_code=400, detail="Missing agent_name, session_id, or files")

    # 段白名单（I-upload-traversal-block 第二层）
    if not _is_safe_path_segment(agent_name):
        raise HTTPException(status_code=400, detail=f"Invalid agent_name: {agent_name}")
    if not _is_safe_path_segment(session_id):
        raise HTTPException(status_code=400, detail=f"Invalid session_id: {session_id}")

    artifact_ids = []
    storage_root = _ARTIFACTS_STORAGE_DIR.resolve()

    for f in files:
        file_path = f.get("path", "")
        data_b64 = f.get("data_b64", "")

        if not file_path:
            logger.warning("artifact upload skip: empty path")
            continue
        if not data_b64:
            logger.warning("artifact upload skip: empty data_b64 path=%s", file_path)
            continue

        # 白名单字符过滤（I-upload-traversal-block 第一层）
        if not _is_safe_relative_path(file_path):
            raise HTTPException(status_code=400, detail=f"Invalid path: {file_path}")

        artifact_id = f"{agent_name}/{session_id}/{file_path}"
        dest = (storage_root / artifact_id).resolve()

        # realpath 越界检查（I-upload-traversal-block 第三层）
        try:
            dest.relative_to(storage_root)
        except ValueError:
            logger.warning("artifact upload traversal blocked id=%s", artifact_id)
            raise HTTPException(status_code=403, detail="Path traversal blocked")

        try:
            content = base64.b64decode(data_b64)
        except (ValueError, base64.binascii.Error) as e:
            raise HTTPException(status_code=400, detail=f"Invalid base64: {e}")

        dest.parent.mkdir(parents=True, exist_ok=True)
        try:
            dest.write_bytes(content)
            artifact_ids.append(artifact_id)
        except OSError as e:
            logger.error("artifact upload write failed id=%s err=%s", artifact_id, e)
            raise HTTPException(status_code=500, detail=f"Write failed: {e}")

    return {"artifact_ids": artifact_ids}


# ── 下载端点 ────────────────────────────────────────────────────────────


@router.get("/{agent_id}/files/{relative_path:path}")
async def get_artifact_legacy(agent_id: str, relative_path: str) -> FileResponse:
    """Legacy route: ``/api/artifacts/{agent_id}/files/{relative_path}``

    Maintained for backward compatibility.  Internally delegates to the
    artifact-ID resolution path.
    """
    return await get_artifact_file(f"{agent_id}/{relative_path}")


@router.get("/{artifact_id:path}")
async def get_artifact_file(artifact_id: str) -> FileResponse:
    """Serve an artifact by its ID.

    Artifact ID format: ``{agent_name}/{session_id}/{file_path}``
    (e.g. ``outfit-assistant/user_abc_client_123/outputs/look.png``).

    Resolution order:
    1. Bridge 本地存储（_ARTIFACTS_STORAGE_DIR）
    2. Local filesystem (``agent_packages_path``) — backward compatible fallback
    """
    # Strategy 1: bridge 本地存储
    stored = (_ARTIFACTS_STORAGE_DIR / artifact_id).resolve()
    try:
        stored.relative_to(_ARTIFACTS_STORAGE_DIR.resolve())
    except ValueError:
        logger.warning("artifact_download.traversal_blocked id=%s", artifact_id)
        raise HTTPException(status_code=403, detail="Path traversal blocked")
    if stored.is_file():
        if stored.suffix.lower() not in _ARTIFACT_SUFFIXES:
            logger.warning("artifact_serve.unsupported id=%s suffix=%s", artifact_id, stored.suffix)
            raise HTTPException(status_code=415, detail="unsupported artifact type")
        return _file_response(stored)

    # Strategy 2: local filesystem fallback (legacy path format)
    _aid = artifact_id.split("/")[0] if "/" in artifact_id else ""
    candidate = _resolve_artifact(artifact_id, agent_pkg_name=_aid)

    if candidate is None or not candidate.is_file():
        logger.warning("artifact_serve.not_found id=%s", artifact_id)
        raise HTTPException(status_code=404, detail="artifact not found")

    if candidate.suffix.lower() not in _ARTIFACT_SUFFIXES:
        logger.warning("artifact_serve.unsupported id=%s suffix=%s", artifact_id, candidate.suffix)
        raise HTTPException(status_code=415, detail="unsupported artifact type")

    return _file_response(candidate)


def _file_response(path: Path) -> FileResponse:
    """Create a FileResponse with proper media type for images."""
    media_type = _image_media_type(path) if path.suffix.lower() in _IMAGE_SUFFIXES else None
    return FileResponse(path, media_type=media_type)
