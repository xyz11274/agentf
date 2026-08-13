"""Shared artifact extraction logic for chat response processing.

Provides:
- ``extract_artifact_infos(content)`` — parses ``ChatChunk.content`` for
  artifact Markdown patterns and returns structured ``ArtifactInfo`` objects.
- ``serialize_artifacts(content)`` — convenience wrapper that extracts and
  JSON-serializes artifacts for database persistence.
- ``publicize_artifact_urls(content)`` — rewrites relative ``/api/artifacts/``
  URLs to public absolute URLs when ``artifact_public_base_url`` is configured.
- ``apply_artifact_output_mode(content, artifacts)`` — rewrites response text
  and artifact metadata per the two orthogonal output-mode switches
  (``artifact_reference``: link | placeholder, ``artifact_payload``: url | base64).

This is consumed by both ``routes/chat.py`` (WebSocket) and
``routes/chat_rest.py`` (REST SSE) to populate the ``ChatChunk.artifacts`` field.
"""

from __future__ import annotations

import json
import logging
import mimetypes
import re
import time
from pathlib import Path

from config import settings
from models import ArtifactInfo
from routes.artifacts import artifact_file_size, read_artifact_base64

logger = logging.getLogger("agentflow.bridge.routes.artifact_extractor")


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
    _t0 = time.time()

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

    # 审计日志：产物提取结果。完整 url 清单落 logger（F7：不截断存储层）。
    # SSE 流每轮 final chunk 会调用一次；同步模式只调一次；持久化路径另见 serialize_artifacts。
    if artifacts:
        logger.info(
            "artifact.extract count=%d urls=%s",
            len(artifacts),
            [a.url for a in artifacts],
        )
    else:
        logger.debug("artifact.extract count=0 (no artifact patterns matched)")
    logger.debug(
        "artifact.extract_done count=%d elapsed=%.3fs",
        len(artifacts), time.time() - _t0,
    )
    return artifacts


def serialize_artifacts(content: str) -> str:
    """Extract structured artifacts from response content and serialize to JSON.

    Returns a JSON string suitable for the ``artifacts`` column in
    ``save_dialogue()``.
    """
    artifacts = extract_artifact_infos(content)
    if not artifacts:
        logger.info("artifact.persist count=0 (nothing to serialize)")
        return "[]"
    serialized = json.dumps([a.model_dump() for a in artifacts], ensure_ascii=False)
    # 审计日志：产物清单即将落盘到 dialogue_rounds.artifacts TEXT 列。
    # 完整清单落 logger（F7），事后可凭此回溯"持久化前产物语义现场"。
    logger.info(
        "artifact.persist count=%d json_len=%d urls=%s",
        len(artifacts),
        len(serialized),
        [a.url for a in artifacts],
    )
    return serialized


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


def apply_artifact_output_mode(content: str, artifacts: list[ArtifactInfo]) -> tuple[str, list[ArtifactInfo]]:
    """Rewrite response text + artifact metadata per the output-mode switches.

    两维正交开关（设计定稿 §3.6）：
      - ``artifact_reference``: ``link``（默认，Markdown 链接）| ``placeholder``（{{artifact:N}} 占位符）
      - ``artifact_payload``:   ``url``（默认，HTTP serve 地址）| ``base64``（内容以 base64 内嵌）

    组合语义：
      - link + url        → 现状，无转换
      - link + base64     → 文本中产物引用替换为 data URI（data:{mime};base64,...），
                            元数据 ``data_base64`` 填充内容
      - placeholder + url → 文本中产物引用替换为 {{artifact:N}}，元数据附 ``placeholder``
      - placeholder + base64 → 占位符 + 元数据附 ``placeholder`` 与 ``data_base64``
                            （方案 3：前端拿到 base64 文本后自行替换占位符）

    调用方在 ``extract_artifact_infos`` 之后、构造对外事件之前调用；
    返回值用于对外输出，持久化路径用同一转换后结果保证 I-3 三路一致。
    """
    reference = getattr(settings, "artifact_reference", "link")
    payload = getattr(settings, "artifact_payload", "url")
    _t0 = time.time()
    if reference == "link" and payload == "url":
        logger.debug("artifact.output_mode_done mode=link+url elapsed=%.3fs", time.time() - _t0)
        return content, artifacts

    # payload=base64：读取产物文件转 base64 填入 data_base64（无法读取时保留 url 形式）。
    # 非图片产物(视频/音频/文档)超过体积阈值时**跳过填充**并打 WARNING——
    # 避免把大视频/大文档的 raw base64 塞进 SSE 元数据（base64 膨胀 ~33% 且不压缩）。
    # 图片不受限(已压缩到 512px,体积可控)。跳过 = 元数据 data_base64 保持空串,
    # 前端 artifactSrc 回退到 url(与 I-8 "内嵌失败保留 url"同语义,不阻断产物交付)。
    # 阈值可配置:settings.artifact_base64_max_bytes(env AF_BR_ARTIFACT_BASE64_MAX_BYTES,默认 1MB,0=不限)。
    max_bytes = getattr(settings, "artifact_base64_max_bytes", 0)
    if payload == "base64":
        for art in artifacts:
            if art.data_base64:
                continue
            if art.type != "image" and max_bytes > 0:
                size = artifact_file_size(art.url)
                if size is not None and size > max_bytes:
                    logger.warning(
                        "artifact.base64.skip_non_image type=%s url=%s size=%d max_bytes=%d "
                        "(keep url form, frontend fetches via serve URL)",
                        art.type, art.url, size, max_bytes,
                    )
                    continue  # 跳过填充,保留 url 形式(I-8)
            art.data_base64 = read_artifact_base64(art.url, art.mime_type)

    if reference == "link":
        # link + base64：仅对 image 做文本内 data URI 内嵌（前端 markdown-it 原生渲染
        # data:image/*，见 MarkdownContent.vue validateLink 白名单）。
        # html/download 保留 serve URL：data:text/html 有 XSS 风险、download 需要文件名/下载
        # 语义，文本内嵌无意义；其内容仍经 data_base64 元数据提供（消费端按需取用）。
        for art in artifacts:
            if art.data_base64 and art.type == "image":
                data_uri = f"data:{art.mime_type};base64,{art.data_base64}"
                content = content.replace(art.url, data_uri)
        return content, artifacts

    # reference == "placeholder": 文本替换为 {{artifact:N}} 占位符，元数据附 placeholder
    for idx, art in enumerate(artifacts, start=1):
        placeholder = f"{{{{artifact:{idx}}}}}"
        art.placeholder = placeholder
        content = content.replace(art.url, placeholder)
    logger.debug("artifact.output_mode_done mode=placeholder+%s elapsed=%.3fs",
                 payload, time.time() - _t0)
    return content, artifacts
