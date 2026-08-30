"""Secure serving of generated chat artifacts from file storage service.

Phase 2: Bridge 自身作为存储中心（见 docs/design/features/sandbox/artifact-storage.md §3.3）。

- 上传：POST /api/artifacts/upload → 路径安全校验 → 写入本地文件系统
- 下载：GET /api/artifacts/{artifact_id} → 从本地文件系统读取
- 不再转发到外部存储服务（沙箱 HTTP server 不再参与产物存储链路）
"""

import base64
import hashlib
import hmac
import io
import logging
import os
import re
import secrets
import threading
from pathlib import Path
from urllib.parse import parse_qsl, unquote, urlencode, urlsplit, urlunsplit

from fastapi import APIRouter, HTTPException, Query
from fastapi.responses import FileResponse
from PIL import Image

from config import settings

logger = logging.getLogger("agentflow.bridge.routes.artifacts")

router = APIRouter(prefix="/api/artifacts", tags=["artifacts"])
_IMAGE_SUFFIXES = {".png", ".jpg", ".jpeg", ".webp", ".gif"}
_DOCUMENT_SUFFIXES = {".pptx", ".ppt", ".docx", ".doc", ".xlsx", ".xls", ".pdf"}
# 媒体后缀与 skill_executor 的 _artifact_suffixes 对齐(文本/JSON 双通道已能识别
# mp4/mov/webm/mp3/wav),bridge 白名单必须同步放行,否则识别了也 415 serve 不了
# (failure_mode_matrix §五 B-video)。
_MEDIA_SUFFIXES = {".mp4", ".mov", ".webm", ".mp3", ".wav"}
_ARTIFACT_SUFFIXES = (
    _IMAGE_SUFFIXES
    | _DOCUMENT_SUFFIXES
    | _MEDIA_SUFFIXES
    | {".html", ".htm", ".css", ".js", ".json", ".svg"}
)

# ── 产物存储根目录 ──────────────────────────────────────────────────────
# 默认与 agent_workspace_path（③ 运行实例目录）同级，可由配置 artifacts_storage_dir 覆盖。
# 三态模型（agent-package-three-state.md）：产物属运行态数据，落点在 ③ workspace 侧；
# 基准挂在 agent_workspace_path.parent（= examples/）保持存储位置稳定，不随固化态目录迁移。
# S5：artifacts_storage_dir 是 BridgeSettings 显式 Field（config.py），空值走 fallback。
_ARTIFACTS_STORAGE_DIR = Path(
    settings.artifacts_storage_dir or
    str(settings.agent_workspace_path.parent / "artifacts_storage")
)
_SIGNING_KEY_LOCK = threading.Lock()
_SIGNING_KEY: bytes | None = None
_SIGNING_KEY_FILE = _ARTIFACTS_STORAGE_DIR / ".signing_key"


def _artifact_signing_key() -> bytes:
    """Load one persistent Bridge-local key used for artifact capabilities."""
    global _SIGNING_KEY
    if _SIGNING_KEY is not None:
        return _SIGNING_KEY
    with _SIGNING_KEY_LOCK:
        if _SIGNING_KEY is not None:
            return _SIGNING_KEY
        configured = os.environ.get("AF_ARTIFACT_SIGNING_KEY", "")
        if configured:
            _SIGNING_KEY = hashlib.sha256(configured.encode("utf-8")).digest()
            return _SIGNING_KEY

        _ARTIFACTS_STORAGE_DIR.mkdir(parents=True, exist_ok=True)
        try:
            raw = _SIGNING_KEY_FILE.read_bytes()
        except FileNotFoundError:
            raw = secrets.token_bytes(32)
            try:
                fd = os.open(
                    _SIGNING_KEY_FILE,
                    os.O_WRONLY | os.O_CREAT | os.O_EXCL,
                    0o600,
                )
            except FileExistsError:
                raw = _SIGNING_KEY_FILE.read_bytes()
            else:
                try:
                    os.write(fd, raw)
                finally:
                    os.close(fd)
        if len(raw) < 32:
            raise RuntimeError("artifact signing key is invalid")
        _SIGNING_KEY = hashlib.sha256(raw).digest()
        return _SIGNING_KEY


def _artifact_signature(artifact_id: str) -> str:
    return hmac.new(
        _artifact_signing_key(),
        artifact_id.encode("utf-8"),
        hashlib.sha256,
    ).hexdigest()


def _valid_artifact_signature(artifact_id: str, signature: str) -> bool:
    return bool(signature) and hmac.compare_digest(
        signature, _artifact_signature(artifact_id),
    )


def sign_artifact_url(url: str) -> str:
    """Attach or replace the capability signature on one artifact URL."""
    parsed = urlsplit(url)
    marker = "/api/artifacts/"
    if marker not in parsed.path:
        return url
    artifact_id = unquote(parsed.path.split(marker, 1)[1])
    query = [(key, value) for key, value in parse_qsl(parsed.query) if key != "sig"]
    query.append(("sig", _artifact_signature(artifact_id)))
    return urlunsplit((
        parsed.scheme,
        parsed.netloc,
        parsed.path,
        urlencode(query),
        parsed.fragment,
    ))


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


# 媒体后缀 → MIME（与 _MEDIA_SUFFIXES 白名单配套；缺失时浏览器无法内联播放）
_MEDIA_MIME = {
    ".mp4": "video/mp4",
    ".mov": "video/quicktime",
    ".webm": "video/webm",
    ".mp3": "audio/mpeg",
    ".wav": "audio/wav",
}


def _media_type(path: Path) -> str | None:
    """按后缀返回媒体 MIME；非媒体后缀返回 None（交给调用方回退到图片/默认）。"""
    return _MEDIA_MIME.get(path.suffix.lower())


def _resolve_artifact(relative_path: str, agent_pkg_name: str = "") -> Path:
    """Resolve a relative artifact path, searching multiple roots.

    Strategy:
    1. Primary: resolve inside ``agent_workspace_path``（③ 运行实例目录，产物落点）。
    2. Fallback: strip session_id segment and try again.
    3. Fallback: project root (legacy paths).
    """
    root = settings.agent_workspace_path.resolve()
    pkg_dir = Path(settings.agent_workspace_dir)

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
async def get_artifact_legacy(agent_id: str, relative_path: str, sig: str = "") -> FileResponse:
    """Legacy route: ``/api/artifacts/{agent_id}/files/{relative_path}``

    Maintained for backward compatibility.  Internally delegates to the
    artifact-ID resolution path.
    """
    legacy_id = f"{agent_id}/files/{relative_path}"
    if not _valid_artifact_signature(legacy_id, sig):
        raise HTTPException(status_code=403, detail="invalid artifact capability")
    artifact_id = f"{agent_id}/{relative_path}"
    return await get_artifact_file(artifact_id, sig=_artifact_signature(artifact_id))


@router.get("/{artifact_id:path}")
async def get_artifact_file(artifact_id: str, sig: str = "") -> FileResponse:
    """Serve an artifact by its ID.

    Artifact ID format: ``{agent_name}/{session_id}/{file_path}``
    (e.g. ``outfit-assistant/user_abc_client_123/outputs/look.png``).

    Resolution order:
    1. Bridge 本地存储（_ARTIFACTS_STORAGE_DIR）
    2. Local filesystem (``agent_packages_path``) — backward compatible fallback
    """
    # Strategy 1: bridge 本地存储
    if not _valid_artifact_signature(artifact_id, sig):
        raise HTTPException(status_code=403, detail="invalid artifact capability")

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
    """Create a FileResponse with proper media type for images/media.

    媒体产物(视频/音频)必须有正确 Content-Type,否则浏览器无法内联播放
    (_MEDIA_SUFFIXES 白名单配套,见 failure_mode_matrix §五 B-video)。
    """
    suffix = path.suffix.lower()
    media_type = (
        _image_media_type(path) if suffix in _IMAGE_SUFFIXES else _media_type(path)
    )
    return FileResponse(path, media_type=media_type)


def artifact_file_size(url: str) -> int | None:
    """Resolve an artifact serve URL to its local file and return byte size.

    Resolution order mirrors :func:`read_artifact_base64`: bridge 本地存储优先,
    legacy filesystem fallback 其次。文件不存在/路径越界返回 ``None``。

    用途:base64 填充前判断非图片文件是否超过体积阈值(避免把大视频/文档
    塞进 SSE 元数据,见 apply_artifact_output_mode 的 artifact_base64_max_bytes)。
    """
    if not url:
        return None
    url = url.split("?")[0]
    m = re.search(r"/api/artifacts/(.+?)/files/(.+)", url)
    if m:
        artifact_id = f"{m.group(1)}/{m.group(2)}"
    else:
        m = re.search(r"/api/artifacts/(.+)", url)
        if not m:
            return None
        artifact_id = m.group(1)

    storage_root = _ARTIFACTS_STORAGE_DIR.resolve()
    stored = (storage_root / artifact_id).resolve()
    try:
        stored.relative_to(storage_root)
    except ValueError:
        return None
    if stored.is_file():
        try:
            return stored.stat().st_size
        except OSError:
            return None

    agent_name = artifact_id.split("/")[0]
    candidate = _resolve_artifact(artifact_id, agent_pkg_name=agent_name)
    if candidate is not None and candidate.is_file():
        try:
            return candidate.stat().st_size
        except OSError:
            return None
    return None


def read_artifact_base64(url: str, mime_type: str | None = None) -> str:
    """Read an artifact referenced by its serve URL and return base64 content.

    Supports both URL shapes:
      ``/api/artifacts/{agent_id}/files/{relative_path}``
      ``/api/artifacts/{artifact_id}``
    (公网化后的绝对 URL 同样支持——从 ``/api/artifacts/`` 开始匹配）。

    Image artifacts are automatically compressed (resize+quality) to keep the
    base64 payload size reasonable for multi-turn dialogue.  Non-image files
    are returned as raw base64 (no compression).

    Resolution order mirrors :func:`get_artifact_file`: bridge 本地存储优先，
    legacy filesystem fallback 其次。文件不存在/路径越界/读取失败返回 ``""``，
    由调用方决定保留 URL 形式（不阻断产物展示）。
    """
    if not url:
        return ""
    url = url.split("?")[0]
    m = re.search(r"/api/artifacts/(.+?)/files/(.+)", url)
    if m:
        artifact_id = f"{m.group(1)}/{m.group(2)}"
    else:
        m = re.search(r"/api/artifacts/(.+)", url)
        if not m:
            return ""
        artifact_id = m.group(1)

    storage_root = _ARTIFACTS_STORAGE_DIR.resolve()
    stored = (storage_root / artifact_id).resolve()
    try:
        stored.relative_to(storage_root)
    except ValueError:
        return ""
    if stored.is_file():
        return _encode_file(stored, mime_type)

    agent_name = artifact_id.split("/")[0]
    candidate = _resolve_artifact(artifact_id, agent_pkg_name=agent_name)
    if candidate is not None and candidate.is_file():
        return _encode_file(candidate, mime_type)
    return ""


# ── 图片压缩 ──────────────────────────────────────────────────────────
# 仅对图片类型做压缩，非图片文件（pdf/pptx 等）返回 raw base64。
# 压缩发生在"读取/返回 base64"时（read_artifact_base64 → _encode_file），
# **不污染存储原图**——存储层始终保留原始文件，压缩仅为传输/上下文减负。
# 开关：settings.artifact_compress_enabled（env AF_BR_ARTIFACT_COMPRESS_ENABLED，默认启用）。
# 压缩参数：
#   MAX_DIMENSION   — 最大边长（px），超出的图片等比缩放到此尺寸（业务传输 512px 足够）
#   JPEG_QUALITY    — JPEG 压缩质量
# 图片压缩大幅减少 base64 体积，避免多轮对话上下文/持久化膨胀。
# 压缩后的图片仍为自包含 data URI，消费端无需额外处理。

_MAX_DIMENSION = 512
_JPEG_QUALITY = 80
_SUPPORTED_IMAGE_EXTS = {".png", ".jpg", ".jpeg", ".webp", ".gif"}


def _encode_file(path: Path | str, mime_type: str | None = None) -> str:
    """Read a file and return its base64 content, compressing images on the fly.

    ``mime_type`` is an optional hint (e.g. ``"image/png"``) — the function
    also falls back to the file extension when ``mime_type`` is ``None``.

    压缩开关关闭（settings.artifact_compress_enabled=false）时跳过压缩,返回 raw base64。
    """
    path = Path(path) if isinstance(path, str) else path
    ext = path.suffix.lower()
    is_image = (mime_type or "").startswith("image/") or ext in _SUPPORTED_IMAGE_EXTS

    if is_image and getattr(settings, "artifact_compress_enabled", True):
        try:
            raw_bytes = path.read_bytes()
            img = Image.open(path)
            # 等比缩放 — 仅当超出最大边长时才缩放
            if img.width > _MAX_DIMENSION or img.height > _MAX_DIMENSION:
                ratio = min(_MAX_DIMENSION / img.width, _MAX_DIMENSION / img.height)
                new_w = int(img.width * ratio)
                new_h = int(img.height * ratio)
                img = img.resize((new_w, new_h), Image.LANCZOS)

            # 统一转 RGB（用于 JPEG 压缩）
            if img.mode in ("RGBA", "LA", "P"):
                background = Image.new("RGB", img.size, (255, 255, 255))
                if img.mode == "P":
                    img = img.convert("RGBA")
                background.paste(img, mask=img.split()[-1] if img.mode == "RGBA" else None)
                img_rgb = background
            elif img.mode != "RGB":
                img_rgb = img.convert("RGB")
            else:
                img_rgb = img

            # 尝试 JPEG 压缩，与原格式取最小者
            # 照片类（JPEG/WebP）→ JPEG 压缩效果好；简单图（PNG 纯色）→ PNG 原格式更小
            buf_jpeg = io.BytesIO()
            img_rgb.save(buf_jpeg, format="JPEG", quality=_JPEG_QUALITY, optimize=True)
            jpeg_b64 = base64.b64encode(buf_jpeg.getvalue()).decode("ascii")

            # 原格式 base64（作为对照）
            raw_b64 = base64.b64encode(raw_bytes).decode("ascii")

            # 选更小的
            if len(jpeg_b64) < len(raw_b64):
                return jpeg_b64
            return raw_b64
        except Exception:
            logger.warning("_encode_file image compress failed path=%s ext=%s", path, ext)
            # 压缩失败 → 退化为 raw base64（不丢数据）
            return base64.b64encode(path.read_bytes()).decode("ascii")

    # 非图片文件：直接 raw base64
    return base64.b64encode(path.read_bytes()).decode("ascii")
