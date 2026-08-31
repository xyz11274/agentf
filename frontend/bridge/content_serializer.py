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


def _artifact_routing_id(user_id: str, client_id: str) -> str:
    """Mirror runtime/C++ artifact routing without adding a package dependency."""
    user_size = len(user_id.encode("utf-8"))
    client_size = len(client_id.encode("utf-8"))
    if user_size > 120 or client_size > 120:
        raise ValueError("user_id and client_id must be at most 120 bytes")
    if "_" not in user_id and "_" not in client_id:
        return f"{user_id}_{client_id}"
    return f"v2__u{user_size}-{user_id}-c{client_size}-{client_id}"


def _compute_upload_dir(agent_id: str, user_id: str, client_id: str) -> Path:
    """Return the per-session workspace upload directory.

    Path: ``{agent_workspace_path}/{agent_id}/workspace/{artifact_routing_id}/upload/``
    （③ workspace 实例内；三态模型见 agent-package-three-state.md）

    Raises ``ValueError`` if ``user_id`` or ``client_id`` is empty — session
    context is required for user-data isolation. No backward-compatible fallback.
    """
    if not user_id or not client_id:
        raise ValueError(
            "session-level upload path requires user_id and client_id, "
            f"got user_id={user_id!r} client_id={client_id!r}"
        )
    routing_id = _artifact_routing_id(user_id, client_id)
    return settings.agent_workspace_path / agent_id / "workspace" / routing_id / "upload"


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
        # agent_root = {agent_workspace_path}/{agent_id}（③ workspace 实例，三态模型）
        agent_root = settings.agent_workspace_path / agent_id
        parts = flatten_to_text(parts, upload_dir, agent_root)
    # Return plain text (no JSON array) — daemon sets this as AF_INPUT
    # and coordinator reads it as the user message.
    texts = [p["text"] for p in parts if p.get("type") == "text"]
    # 非 text part(如未来新增 video/file 类型)会在此被丢弃——不得静默(M3):
    # 打 WARNING 说明类型与原因,避免"用户传了内容但 agent 完全不知道"。
    dropped = [p.get("type") for p in parts if p.get("type") != "text"]
    if dropped:
        logger.warning(
            "content_serializer.drop_non_text types=%s — 当前仅支持 text 与 image_url,"
            "其余 part 类型未落盘且未进入对话上下文",
            sorted(set(dropped)),
        )
    return "\n".join(texts) if texts else content if isinstance(content, str) else ""


def serialize_content_for_storage(
    content: str | list[ContentPart] | list[dict[str, Any]],
) -> str:
    """I-REQ（03-bridge-session-persistence.md §1.3）：把用户原始请求序列化为存储形态。

    原样保留 content（含图片 base64 data URI）——**不得**在此落盘或替换为派生路径文本。
    落盘文件可能被沙箱回收/workspace 清理而丢失，base64 未存则原始输入不可重建。
    字符串原样返回；content part list 序列化为 JSON 字符串（含 base64 真相源）。

    对应消费侧：:func:`derive_history_user_messages`（I-DERIV 消费时派生路径文本）。
    """
    if isinstance(content, str):
        return content
    parts = normalize_content(content)
    return json.dumps(parts, ensure_ascii=False)


def display_text_from_stored(stored: str) -> str:
    """展示层：把存储形态（原始 content JSON 或纯文本）展平为可读文本。

    code-review P3 修复（2026-08-15）：user_input 存原始 content JSON（含 base64）后，
    历史消息 API / 会话自动标题若直接渲染 user_input 会显示 JSON 乱码（含大段 base64）。
    本函数用于**展示层**（get_messages 历史展示、自动标题）：JSON 形态 content parts →
    提取 text part 文本拼接；纯文本 → 原样。

    **注意**：不得用于 LLM 上下文拼接（get_context）——那必须保持原始 content 供
    :func:`derive_history_user_messages` 派生路径文本（I-DERIV）。
    """
    if not isinstance(stored, str):
        return stored
    try:
        parts = json.loads(stored)
    except (json.JSONDecodeError, TypeError):
        return stored
    if isinstance(parts, list) and all(
        isinstance(p, dict) for p in parts
    ):
        texts = [p.get("text", "") for p in parts if p.get("type") == "text"]
        if texts:
            return "\n".join(texts)
    return stored


def derive_history_user_messages(
    history_messages: list[dict[str, Any]],
    agent_id: str = "",
    user_id: str = "",
    client_id: str = "",
) -> list[dict[str, Any]]:
    """I-DERIV（03-bridge-session-persistence.md §1.3）：消费时把历史 user 消息的
    原始 content（JSON 序列化，含 base64）派生为路径文本（"用户上传了图片，路径: ..."）。

    存储层保存原始 content（真相源）；拼 daemon messages 前调用本函数重新落盘并派生
    路径文本——LLM 上下文需要路径才能直接用工具处理图片，base64 只留作真相源。
    纯文本历史消息原样保留；assistant / system 消息原样保留。

    派生幂等（F3 I-4）：``_land_image`` 内容寻址（sha256[:16]，``if not target.exists()``
    才写）保证同一 base64 → 同一路径 → 同一文本，重复处理不叠加副作用。
    """
    out: list[dict[str, Any]] = []
    for msg in history_messages:
        if msg.get("role") != "user":
            out.append(msg)
            continue
        content = msg.get("content", "")
        # 尝试解析为 content part array（存储形态）
        try:
            parsed = json.loads(content)
        except (json.JSONDecodeError, TypeError):
            parsed = None
        # 仅当数组含 image_url 才派生——text-only 数组（用户纯文本恰好是 JSON 形态）
        # round-trip 后文本一致，无需派生；且避免把用户粘贴的 JSON 文本误当存储形态改写
        if (isinstance(parsed, list) and parsed
                and all(
                    isinstance(p, dict) and p.get("type") in ("text", "image_url")
                    for p in parsed
                )
                and any(p.get("type") == "image_url" for p in parsed)):
            derived_text = prepare_content_for_daemon(
                parsed, agent_id=agent_id, user_id=user_id, client_id=client_id,
            )
            out.append({"role": "user", "content": derived_text})
        else:
            # 纯文本历史消息（含 text-only JSON 形态）：原样保留
            out.append(msg)
    return out
