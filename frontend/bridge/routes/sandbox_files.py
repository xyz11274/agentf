"""AgentFlow Bridge — 沙箱产物文件端点。

双通道回退由 daemon 内部处理，Bridge 只做代理，不感知 sandbox 实现细节。
对齐 contracts/bridge-sandbox-files.md + lld.md T063。
"""

from __future__ import annotations

import base64
import logging

from fastapi import APIRouter, HTTPException, Query, Request
from fastapi.responses import Response

logger = logging.getLogger(__name__)

router = APIRouter(prefix="/v1/sessions", tags=["sandbox"])

# 运行时 ArtifactStore 实例（由 lifespan 注入），简化：全局单例
_artifact_store = None


def set_artifact_store(store):
    """设置全局 ArtifactStore 实例（由 lifespan 注入）。"""
    global _artifact_store
    _artifact_store = store


@router.get("/{session_id}/sandbox/files")
async def get_sandbox_file(
    request: Request,
    session_id: str,
    path: str = Query(..., description="沙箱内文件相对路径"),
):
    """读取沙箱内或快照中的文件。

    双通道回退由 daemon 内部处理：
    1. 优先调 daemon RPC sandbox.read_file 获取文件
    2. RPC 失败时回退到 ArtifactStore 快照
    """
    # ---- 通道 1：daemon RPC（daemon 内部处理双通道回退） ----
    daemon_client = getattr(request.app.state, "daemon_client", None)
    if daemon_client is not None:
        try:
            result = await daemon_client.send_request(
                "sandbox.read_file",
                {"sandbox_id": session_id, "path": path},
            )
            if result and result.get("found"):
                content_b64 = result.get("content_b64", "")
                if content_b64:
                    data = base64.b64decode(content_b64)
                    # 根据文件扩展名推断 content-type
                    content_type = _guess_content_type(path)
                    return Response(content=data, media_type=content_type)
                # 纯文本内容
                content = result.get("content", "")
                if content:
                    return Response(content=content, media_type="text/plain; charset=utf-8")
        except Exception as exc:
            logger.warning("sandbox.read_file RPC failed: %s", exc)

    # ---- 通道 2：ArtifactStore 快照（回退） ----
    if _artifact_store is None:
        raise HTTPException(status_code=500, detail="INTERNAL: ArtifactStore not initialized")

    if not _artifact_store.has_snapshot(session_id):
        raise HTTPException(
            status_code=410,
            detail={
                "error": "SANDBOX_AND_SNAPSHOT_GONE",
                "message": f"File '{path}' not available for session '{session_id}'",
            },
        )

    lines = _artifact_store.read_snapshot(session_id, path)
    if not lines:
        raise HTTPException(
            status_code=404,
            detail={
                "error": "FILE_NOT_FOUND",
                "message": f"File '{path}' not found for session '{session_id}'",
            },
        )

    return {"content": "\n".join(lines), "source": "snapshot"}


def _guess_content_type(path: str) -> str:
    """根据文件扩展名推断 content-type。"""
    ext = path.rsplit(".", 1)[-1].lower() if "." in path else ""
    return {
        "png": "image/png",
        "jpg": "image/jpeg",
        "jpeg": "image/jpeg",
        "gif": "image/gif",
        "svg": "image/svg+xml",
        "webp": "image/webp",
        "html": "text/html; charset=utf-8",
        "htm": "text/html; charset=utf-8",
        "json": "application/json",
        "pdf": "application/pdf",
        "csv": "text/csv; charset=utf-8",
        "txt": "text/plain; charset=utf-8",
        "md": "text/markdown; charset=utf-8",
        "yaml": "text/yaml; charset=utf-8",
        "yml": "text/yaml; charset=utf-8",
        "xml": "application/xml",
        "zip": "application/zip",
    }.get(ext, "application/octet-stream")