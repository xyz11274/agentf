"""Artifact extraction / relocation / upload pipeline.

Split from skill_executor.py (2026-08-14).  All public symbols are re-exported
by skill_executor so existing ``from agentflow_runtime.skill_executor import X``
call sites continue to work unchanged.

Design constraints:
  - AGENT_PACKAGE_DIR remains the single source of truth in skill_executor.
    This module never defines its own AGENT_PACKAGE_DIR global; instead each
    function reads it lazily via ``_se.AGENT_PACKAGE_DIR``.
  - No business logic / log format / protocol changes — pure structural split.
"""

import hashlib
import json
import logging
import os
import re
import sys
import time
from pathlib import Path

from agentflow_runtime.common.runtime_context import (
    artifact_session_id as _artifact_session_id,
)
from agentflow_runtime.executors.factory import sandbox_workspace_prefix

logger = logging.getLogger(__name__)

# ── Structured / dual-channel extraction ─────────────────────────────
# 防御性解析：malformed JSON、非 dict、非 list、非 string items → 返回空
# 字段优先级（or-chain 短路）：artifacts > files > outputs > result > path
_ARTIFACT_JSON_FIELDS = ("artifacts", "files", "outputs", "result", "path")

# I-6 boundary: directories that hold user-uploaded inputs, never skill-generated artifacts.
# Match against resolved absolute paths only; keep lowercase for case-insensitive compare.
# Supports both old (workspace/upload/) and new session-level (workspace/{session}/upload/) paths.
# 注意: 裸 "/workspace/" 不得作为 marker —— 宿主机上 agent 包内的用户会话
# 工作目录 workspace/{user_id}_{client_id}/ 含该子串,会误伤 LLM 生成在会话
# 目录下的产物(2026-08-15 前端会话实测: 图片生成到 workspace/{session}/assets/
# 被误判为上传输入 → skipped_upload → 相对路径未重写 → 前端图片无法显示)。
# session 格式 /workspace/{user}_{client}/upload/ 与 legacy /workspace/upload/
# 均含 /upload/,已由 _is_user_upload_input 的 has_workspace+has_upload 分支覆盖,
# 此处只需保留无 /workspace/ 前置的独立上传目录(如 /uploads/)作兜底。
_USER_UPLOAD_INPUT_MARKERS = ("/workspace/upload/", "/uploads/")


def _pkg_dir() -> str:
    """Read AGENT_PACKAGE_DIR from skill_executor (single source of truth).

    Lazy import avoids circular dependency at module-load time.
    """
    from agentflow_runtime import skill_executor as _se
    return _se.AGENT_PACKAGE_DIR


def _emit_artifact_storage_result(
    status: str,
    *,
    agent_name: str,
    session_id: str,
    entries: list[dict],
    attempted_count: int = 0,
    uploaded_count: int = 0,
    returned_artifact_ids: list[str] | None = None,
    error_type: str = "",
    error: str = "",
) -> None:
    """以 fail-open Hook 记录产物持久化事实，不改变上传返回语义。"""
    try:
        from agentflow_runtime import events as hook_events
        from agentflow_runtime.hooks import trigger as hook_trigger

        hook_trigger(hook_events.ARTIFACT_STORAGE_RESULT, {
            "dialogue_id": os.environ.get("AF_DIALOGUE_ID", ""),
            "skill_name": os.environ.get("AF_SKILL_NAME", ""),
            "exec_skill_key": os.environ.get("AF_EXEC_SKILL_KEY", ""),
            "status": status,
            "agent_name": agent_name,
            "artifact_session_id": session_id,
            "attempted_count": attempted_count,
            "uploaded_count": uploaded_count,
            "returned_artifact_ids": returned_artifact_ids or [],
            "entries": entries,
            "error_type": error_type,
            "error": error,
        })
    except Exception as exc:  # pragma: no cover - 仅导入/事件层异常
        logger.warning("artifact.storage_result_hook_failed err=%s", exc)


def extract_artifact_paths(text: str) -> list[str]:
    """Extract generated image URLs/paths from a tool result.

    Handles three formats:
      - HTTP(S) URLs:         https://cdn.example/look.jpg
      - Absolute paths:       /home/user/outfit.png
      - Relative paths:       .agentfoundry/.../file.png, assets/outfit.png

    Relative paths are resolved against AGENT_PACKAGE_DIR (the cwd for tool execution).

    I-6 boundary (see artifact-output-pipeline.md §5): artifacts must be files
    the skill *generated this turn*, not inputs it consumed / referenced / read.
    We guard by excluding known user-upload input directories
    (workspace/upload/, uploads/). The framework copies generated files into
    outputs/ via _ensure_artifacts_in_package, so uploads/ never legitimately
    holds a generated artifact.
    """
    pattern = (
        r"(?:"
        r"https?://[^\s\"'\\]+"          # http(s) URLs
        r"|/[^\s\"'\\]+"                 # absolute paths
        r"|(?:\.\.?)?[^\s\"'\\:]+/[^\s\"'\\]+"  # relative paths with at least one /
        r")"
        # 后缀与 extract_structured_artifacts 的 _artifact_suffixes 对齐,
        # 消除双通道不一致(failure_mode_matrix §五 B-video:文本通道曾无法识别
        # mp4;html/htm 同为报告产物,文本通道曾无法识别 → coordinator 收不到
        # html_reports,只能走 downloads 分类)。
        r"\.(?:png|jpe?g|webp|gif|html?|mp4|mov|webm|mp3|wav|pptx?|docx?|xlsx?|pdf)"
    )
    candidates = re.findall(pattern, text, flags=re.IGNORECASE)
    seen: list[str] = []
    skipped_upload: list[str] = []          # I-6 boundary audit trail
    skipped_missing: list[str] = []
    skipped_empty: list[str] = []           # exists but 0 bytes (下载/写入异常)
    for item in candidates:
        if item.startswith(("http://", "https://")):
            # External URL — no upload-dir guard applies, but still skip
            # obvious upload endpoint paths (defensive; not the main path).
            if "/upload/" in item or "/uploads/" in item:
                skipped_upload.append(item)
                continue
            seen.append(item)
        elif item.startswith("/"):
            # Absolute path — reject if it lives under an upload input directory
            if _is_user_upload_input(item):
                skipped_upload.append(item)
                continue
            # e2b 模式：沙箱内产物（/home/user/workspace/...）宿主机上不存在，
            # 但文件在沙箱内、由 _ensure_artifacts_in_package 从沙箱拉回。放行
            # （与 extract_structured_artifacts 的 ws_prefix 放行保持一致，
            #  双通道对齐；缺此放行 → HTML/图片被 skipped_missing 丢弃，
            #  前端 ArtifactList 收不到下载入口，2026-08-14 实测回归）。
            ws_prefix = sandbox_workspace_prefix()
            if ws_prefix and item.startswith(ws_prefix):
                seen.append(item)
                continue
            if os.path.isfile(item):
                if os.path.getsize(item) > 0:
                    seen.append(item)
                else:
                    skipped_empty.append(item)
                    logger.warning("artifact.skipped_empty path=%s size=0", item)
            else:
                skipped_missing.append(item)
        else:
            # Relative path — resolve against AGENT_PACKAGE_DIR
            abs_path = os.path.normpath(os.path.join(_pkg_dir(), item))
            if _is_user_upload_input(abs_path):
                skipped_upload.append(abs_path)
                continue
            if os.path.isfile(abs_path):
                if os.path.getsize(abs_path) > 0:
                    seen.append(abs_path)
                else:
                    skipped_empty.append(abs_path)
                    logger.warning("artifact.skipped_empty path=%s size=0", abs_path)
    # I-6 audit log: 守护点证据。完整清单落 stderr，不截断（F7）。
    print(
        f"[flow] artifact.collect candidates={len(candidates)} "
        f"admitted={len(seen)} skipped_upload={len(skipped_upload)} "
        f"skipped_missing={len(skipped_missing)} "
        f"skipped_empty={len(skipped_empty)}",
        file=sys.stderr,
    )
    if skipped_upload:
        print(
            "[flow] artifact.skipped_upload (I-6 input boundary) "
            + json.dumps(skipped_upload, ensure_ascii=False),
            file=sys.stderr,
        )
    if skipped_missing:
        print(
            "[flow] artifact.skipped_missing (file not on disk) "
            + json.dumps(skipped_missing, ensure_ascii=False),
            file=sys.stderr,
        )
    if skipped_empty:
        print(
            "[flow] artifact.skipped_empty (file is 0 bytes) "
            + json.dumps(skipped_empty, ensure_ascii=False),
            file=sys.stderr,
        )
    return list(dict.fromkeys(seen))


def extract_structured_artifacts(result_json: str) -> list[str]:
    """Extract artifact paths from a JSON tool result's structured fields.

    Parses the JSON, looks for ``artifacts``/``files``/``outputs`` (first
    truthy field wins, or-chain short-circuit), and returns the list of
    artifact paths. Defensive: malformed JSON / non-dict / non-list /
    non-string items all return ``[]``.

    Relative paths are resolved against ``AGENT_PACKAGE_DIR``.
    User-upload inputs (workspace/upload/, uploads/) are rejected (I-6).
    Non-existent files are skipped.
    """
    if not result_json or not result_json.strip():
        return []
    _t0 = time.monotonic()
    try:
        data = json.loads(result_json)
    except (ValueError, TypeError):
        _ms = (time.monotonic() - _t0) * 1000
        print(f"[timing] artifact.extract_structured duration_ms={_ms:.1f}", file=sys.stderr)
        return []
    if not isinstance(data, dict):
        _ms = (time.monotonic() - _t0) * 1000
        print(f"[timing] artifact.extract_structured duration_ms={_ms:.1f}", file=sys.stderr)
        return []

    # Pick the first truthy field (or-chain short-circuit semantics)
    items: list = []
    for field in _ARTIFACT_JSON_FIELDS:
        val = data.get(field)
        if isinstance(val, list) and val:
            items = val
            break

    if not items:
        _ms = (time.monotonic() - _t0) * 1000
        print(f"[timing] artifact.extract_structured duration_ms={_ms:.1f}", file=sys.stderr)
        return []

    # Resolve to absolute path. Callers overriding AGENT_PACKAGE_DIR must patch
    # THIS module's globals (entry file's override does NOT propagate here —
    # closure binds at function-def time).
    package_root = os.path.realpath(_pkg_dir())

    # Artifact suffix whitelist (mirror append_artifacts_markdown semantics):
    # only files with these suffixes are "generated artifacts" — inputs like
    # readme.txt / config.yaml are NOT artifacts (I-6 boundary).
    _artifact_suffixes = {
        ".png", ".jpg", ".jpeg", ".webp", ".gif",  # images
        ".html", ".htm",                            # html reports
        ".pdf", ".pptx", ".docx", ".xlsx",          # office downloads
        ".mp4", ".mp3", ".wav", ".webm",            # media
        ".svg", ".bmp", ".tiff",                    # more images
        ".csv", ".json", ".yaml", ".yml",           # structured reports (legacy)
    }

    extracted: list[str] = []
    for item in items:
        if not isinstance(item, str) or not item:
            continue
        # HTTP URL: pass through unchanged (no suffix check for URLs)
        if item.startswith(("http://", "https://")):
            # I-6: skip obvious upload endpoint paths
            if "/upload/" in item or "/uploads/" in item:
                continue
            extracted.append(item)
            continue
        # Absolute or relative path: resolve + guard
        if os.path.isabs(item):
            resolved = os.path.normpath(item)
        else:
            resolved = os.path.normpath(os.path.join(package_root, item))
        if _is_user_upload_input(resolved):
            continue  # I-6 boundary
        # Suffix whitelist: reject non-artifact files (readme.txt, config.yaml, ...)
        if Path(resolved).suffix.lower() not in _artifact_suffixes:
            continue
        # e2b 模式：沙箱内产物（/home/user/workspace/...）宿主机上不存在，
        # 但文件在沙箱内、由 _ensure_artifacts_in_package 从沙箱拉回。放行。
        ws_prefix = sandbox_workspace_prefix()
        if ws_prefix and resolved.startswith(ws_prefix):
            extracted.append(resolved)
            continue
        if not os.path.isfile(resolved):
            # Local/Docker Sandbox files do not exist on the host yet. A
            # structured tool field is explicit enough to defer validation to
            # _ensure_artifacts_in_package, which pulls it through read_file.
            if os.environ.get("AF_SANDBOX_ENDPOINT"):
                extracted.append(resolved)
            continue
        extracted.append(resolved)

    _ms = (time.monotonic() - _t0) * 1000
    print(f"[timing] artifact.extract_structured duration_ms={_ms:.1f}", file=sys.stderr)
    return list(dict.fromkeys(extracted))


def extract_all_artifacts(
    result_json: str,
    *,
    excluded_text_paths: set[str] | list[str] | tuple[str, ...] = (),
) -> list[str]:
    """Dual-channel merge: structured extraction + text regex extraction.

    Combines ``extract_structured_artifacts`` (artifacts/files/outputs JSON
    fields) with ``extract_artifact_paths`` (regex over stdout/text) and
    dedupes preserving first-seen order.
    """
    _t0 = time.monotonic()
    structured = extract_structured_artifacts(result_json)
    text_channel = extract_artifact_paths(result_json)
    excluded = set(excluded_text_paths)
    skipped_inputs = [path for path in text_channel if path in excluded]
    if skipped_inputs:
        print(
            "[flow] artifact.skipped_input_reference "
            + json.dumps(skipped_inputs, ensure_ascii=False),
            file=sys.stderr,
        )
        text_channel = [path for path in text_channel if path not in excluded]

    merged: list[str] = []
    seen: set[str] = set()
    for path in structured + text_channel:
        if path not in seen:
            seen.add(path)
            merged.append(path)
    _ms = (time.monotonic() - _t0) * 1000
    print(f"[timing] artifact.extract_all duration_ms={_ms:.1f}", file=sys.stderr)
    return merged


def _is_user_upload_input(resolved_path: str) -> bool:
    """Return True if the given (resolved) path lives under a user-upload input directory.

    Used by extract_artifact_paths to enforce I-6: skill-consumed inputs must not
    be reported as generated artifacts. Case-insensitive on the directory markers
    because macOS / Linux paths are not reliably canonical.

    Matches both:
      - Legacy:  ``/workspace/upload/``
      - Session: ``/workspace/{user_id}_{client_id}/upload/`` (requires BOTH markers)
    """
    lowered = resolved_path.lower()
    # Session-level isolation path: must contain /workspace/ and /upload/ together
    has_workspace = "/workspace/" in lowered
    has_upload = "/upload/" in lowered
    if has_workspace and has_upload:
        # Verify /workspace/ comes before /upload/ (both in path, workspace precedes upload)
        ws_idx = lowered.index("/workspace/")
        up_idx = lowered.index("/upload/")
        if ws_idx < up_idx:
            return True
    # e2b 沙箱 workspace（/home/user/workspace/...）不是宿主机 upload 目录，
    # 但含 /workspace/ 子串会被下方 /workspace/ marker 误判为上传输入（I-6 误伤）。
    # e2b 模式下真正的上传输入在沙箱内 /home/user/workspace/{session}/upload/ 等，
    # 已由上方 has_workspace+has_upload 分支覆盖；纯沙箱 workspace 路径直接放行。
    ws_prefix = sandbox_workspace_prefix()
    if ws_prefix and resolved_path.startswith(ws_prefix):
        return False
    return any(marker in lowered for marker in _USER_UPLOAD_INPUT_MARKERS)


def _ensure_artifacts_in_package(artifacts: list[str]) -> list[str]:
    """Copy artifacts into package outputs/ and return updated paths.

    The Bridge artifact endpoint only serves files under agent_packages_path.
    This function copies ALL artifact files (whether inside or outside the
    package) to AGENT_PACKAGE_DIR/outputs/{session_id}/, giving a single
    predictable location regardless of where the LLM saved them. The session_id
    subdirectory ensures the local file path matches the URL format
    (/api/artifacts/{agent_name}/{session_id}/{file_path}), so Bridge fallback
    resolution does not need to strip session_id segments. Avoids issues with:
      - Hidden directories (.agentfoundry/) in the URL path
      - Files saved outside the package (/tmp/...)
      - Path resolution mismatches between daemon and Bridge processes

    Sandbox fallback: if a file doesn't exist on the host filesystem but
    AF_SANDBOX_ENDPOINT is set (sandbox endpoint), try to download it
    from the sandbox via read_file tool before giving up.
    """
    # Lazy import: tests monkeypatch skill_executor._download_from_sandbox(_e2b),
    # so we must resolve these via skill_executor's module namespace, not ours.
    from agentflow_runtime import skill_executor as _se
    result = []
    skipped_missing: list[str] = []
    skipped_copy_err: list[str] = []
    _t0 = time.monotonic()
    pkg_root = Path(_pkg_dir()).resolve()
    # artifact 路由键用 {user_id}_{client_id}（artifact-storage.md §2.2），不是 AF_SESSION_ID
    # （后者是执行槽位 0/1/2，缺用户维度会导致不同用户产物撞路径）。
    sess_id = _artifact_session_id()
    outputs_dir = pkg_root / "outputs" / sess_id if sess_id else pkg_root / "outputs"
    outputs_dir.mkdir(parents=True, exist_ok=True)
    kept_in_place: list[str] = []   # 已在 package 内，无需复制
    copied_to_outputs: list[str] = []  # 复制到 outputs/
    total_bytes = 0
    claimed_destinations: dict[Path, str] = {}

    def _claim_output_destination(filename: str, source: str) -> Path:
        """Keep overwrite semantics across calls, but never collide within one call."""
        base = outputs_dir / filename
        previous_source = claimed_destinations.get(base)
        if previous_source is None or previous_source == source:
            claimed_destinations[base] = source
            return base
        stem = Path(filename).stem
        suffix = Path(filename).suffix
        index = 1
        while True:
            candidate = outputs_dir / f"{stem}_{index}{suffix}"
            previous_source = claimed_destinations.get(candidate)
            if previous_source is None or previous_source == source:
                claimed_destinations[candidate] = source
                return candidate
            index += 1

    # 沙箱 endpoint，用于下载宿主机上不存在的文件（沙箱 HTTP server 的 /exec + read_file）
    sandbox_url = os.environ.get("AF_SANDBOX_ENDPOINT", "")
    for path in artifacts:
        # HTTP(S) URLs are external resources — pass through unchanged.
        # Pre-fix bug: Path('https://...').resolve() → not a file → silently skipped.
        if path.startswith(("http://", "https://")):
            result.append(path)
            continue
        p = Path(path).resolve()
        if not p.is_file():
            # e2b 模式：产物在沙箱内（write_file 写入 /home/user/workspace/...），
            # 宿主机不存在 → 用 e2b SDK 通道（envd /files 端点）拉回宿主机
            # outputs/{session_id}/{name}，后续 kept_in_place 直接可服务。
            # （e2b 下 HTTP server 通道不通——cubeproxy 不转发 8080，见 _download_from_sandbox）
            ws_prefix = sandbox_workspace_prefix()
            # macOS 上 Path.resolve() 把 /home/user/workspace/... 展开为
            # /System/Volumes/Data/home/user/workspace/...,与 ws_prefix 前缀
            # 不匹配 → 误走 HTTP 通道(cubeproxy 502,2026-08-14 实测)。
            # 按 normpath 后的原始沙箱路径判断前缀,并把原始沙箱路径传给
            # e2b SDK 拉回(沙箱侧按该路径寻址,resolve 展开路径会查无此文件)。
            raw_path = os.path.normpath(path)
            if ws_prefix and raw_path.startswith(ws_prefix) and sandbox_url:
                dest = _claim_output_destination(p.name, raw_path)
                if _se._download_from_sandbox_e2b(raw_path, dest):
                    p = dest  # 拉回成功 → 按包内文件处理（kept_in_place）
            elif sandbox_url:
                # 非 e2b（Local/Docker）：HTTP server 通道
                _se._download_from_sandbox(p, sandbox_url)
            if not p.is_file():
                skipped_missing.append(path)
                continue  # skip non-existent files rather than returning dead URLs
        file_size = p.stat().st_size
        total_bytes += file_size
        try:
            p.relative_to(pkg_root)
        except ValueError:
            pass
        else:
            # Package-local artifacts are already publishable by Bridge. Keep
            # their directory hierarchy so HTML/CSS relative references and
            # same-named files remain stable.
            try:
                p.relative_to(outputs_dir)
            except ValueError:
                pass
            else:
                claimed_destinations.setdefault(p, str(p))
            result.append(str(p))
            kept_in_place.append(f"{p.name}({file_size}B)")
            continue
        dest = _claim_output_destination(p.name, str(p))
        # 多轮对话覆盖策略(2026-08-14 用户确认):同一 session 内同名产物直接覆盖
        # outputs/ 下旧文件(历史消息 URL 内容变为最新版,接受该语义)；但同一次
        # 收集中的不同源文件不得互相覆盖，后到者使用 _N 后缀。
        # 内容相同则跳过重写(幂等);内容不同则覆盖为最新版。
        if not dest.exists() or dest.read_bytes() != p.read_bytes():
            try:
                dest.write_bytes(p.read_bytes())
            except OSError as exc:
                skipped_copy_err.append(f"{path} -> {dest}: {exc}")
                continue  # skip files that can't be copied
        result.append(str(dest))
        copied_to_outputs.append(f"{dest.name}({file_size}B)")
    # 可观测性审计日志（F7：不截断存储层，便于 e2b 等远端沙箱模式排查）
    _sandbox_mode = os.environ.get("AF_SANDBOX_MODE", "unknown")
    _storage_url = os.environ.get("AF_ARTIFACT_STORAGE_URL", "")
    print(
        f"[flow] artifact.relocate input={len(artifacts)} relocated={len(result)} "
        f"total_bytes={total_bytes} sandbox_mode={_sandbox_mode} "
        f"storage_url={'set' if _storage_url else 'unset'} "
        f"kept_in_place={len(kept_in_place)} copied={len(copied_to_outputs)} "
        f"skipped_missing={len(skipped_missing)} copy_err={len(skipped_copy_err)}",
        file=sys.stderr,
    )
    if kept_in_place:
        print(
            "[flow] artifact.relocate_kept_in_place "
            + json.dumps(kept_in_place, ensure_ascii=False),
            file=sys.stderr,
        )
    if copied_to_outputs:
        print(
            "[flow] artifact.relocate_copied "
            + json.dumps(copied_to_outputs, ensure_ascii=False),
            file=sys.stderr,
        )
    if skipped_missing:
        print(
            "[flow] artifact.relocate_missing " + json.dumps(skipped_missing, ensure_ascii=False),
            file=sys.stderr,
        )
    if skipped_copy_err:
        print(
            "[flow] artifact.relocate_copy_err " + json.dumps(skipped_copy_err, ensure_ascii=False),
            file=sys.stderr,
        )

    # 自动上传到文件存储服务（Phase 2: bridge 自身作为存储中心）
    _upload_artifacts_to_storage(result)

    _ms = (time.monotonic() - _t0) * 1000
    print(f"[timing] artifact.ensure_in_package duration_ms={_ms:.1f}", file=sys.stderr)
    return result


def _download_from_sandbox(local_path: Path, sandbox_url: str) -> None:
    """Download a generated artifact from a Local/Docker Sandbox.

    e2b uses the SDK-specific download path because its cubeproxy does not
    forward this HTTP server. Local/Docker use POST /artifact/download;
    the server returns base64-encoded raw bytes after applying Artifact-type,
    upload-input and workspace-boundary checks.
    """
    import base64 as _base64
    import json as _json
    import urllib.request
    from agentflow_runtime.security.local_sandbox_auth import local_sandbox_headers, local_sandbox_urlopen

    _t0 = time.monotonic()
    try:
        # 将绝对路径转为相对 AGENT_PACKAGE_DIR 的相对路径（沙箱侧按 workdir 解析）
        pkg_root = Path(_pkg_dir()).resolve()
        try:
            rel_path = str(local_path.resolve().relative_to(pkg_root))
        except ValueError:
            rel_path = str(local_path)
        body = _json.dumps({"path": rel_path}).encode()
        req = urllib.request.Request(
            f"{sandbox_url.rstrip('/')}/artifact/download",
            data=body,
            headers={"Content-Type": "application/json", **local_sandbox_headers(sandbox_url)},
            method="POST",
        )
        opener = local_sandbox_urlopen if os.environ.get("AF_SANDBOX_MODE") == "local" else urllib.request.urlopen
        with opener(req, timeout=10) as resp:
            result = _json.loads(resp.read().decode("utf-8"))

        content_b64 = result.get("content_b64")
        if not isinstance(content_b64, str):
            raise ValueError("Sandbox artifact response has no content_b64")
        content = _base64.b64decode(content_b64, validate=True)
        expected_size = result.get("size")
        if not isinstance(expected_size, int) or expected_size != len(content):
            raise ValueError(
                f"Sandbox artifact size mismatch: expected={expected_size} actual={len(content)}"
            )

        local_path.parent.mkdir(parents=True, exist_ok=True)
        local_path.write_bytes(content)
        logger.info(
            "artifact.download_from_sandbox path=%s size=%d",
            local_path,
            len(content),
        )
    except Exception as e:
        logger.warning(
            "artifact.download_from_sandbox.error path=%s err=%s",
            local_path,
            e,
        )
    finally:
        _ms = (time.monotonic() - _t0) * 1000
        print(f"[timing] artifact.download_from_sandbox duration_ms={_ms:.1f}", file=sys.stderr)


def _download_from_sandbox_e2b(sandbox_path: str, local_path: Path) -> bool:
    """e2b 模式：用 e2b SDK 通道（envd /files 端点）从沙箱拉回产物到宿主机。

    e2b 沙箱 HTTP server 通道（_download_from_sandbox）当前不通（cubeproxy 不转发
    8080，HTTP 502，见 artifact-storage.md §1.3）。e2b 下唯一可用通道是 e2b SDK
    直连 envd /files 端点（E2BExecutor.download_to_host，与 _download_images 一致，
    I-binary-safe 取原始 bytes）。

    Args:
        sandbox_path: 沙箱内绝对路径（/home/user/workspace/...）。
        local_path: 宿主机目标路径（outputs/{session_id}/{name}）。

    Returns:
        True 拉回成功；False 失败（调用方走 skipped_missing 降级）。
    """
    sandbox_url = os.environ.get("AF_SANDBOX_ENDPOINT", "")
    if not sandbox_url:
        logger.warning("artifact.download_e2b.skip: no AF_SANDBOX_ENDPOINT path=%s",
                        sandbox_path)
        return False
    try:
        from agentflow_runtime.executors.factory import create_executor
        executor = create_executor(sandbox_mode="e2b", sandbox_endpoint=sandbox_url)
        return executor.download_to_host(sandbox_path, str(local_path))
    except Exception as e:
        logger.warning("artifact.download_e2b.error path=%s err=%s",
                        sandbox_path, e)
        return False


def _upload_artifacts_to_storage(artifact_paths: list[str]) -> None:
    """Upload artifacts to bridge storage center (POST /api/artifacts/upload).

    Phase 2: bridge 自身作为存储中心（见 docs/design/features/sandbox/artifact-storage.md §3.3）。
    不再上传到沙箱 HTTP server——沙箱 HTTP server 不参与产物存储链路。

    失败语义（与 docs/design/features/sandbox/artifact-storage.md §7 对齐）：
    - 配置缺失/无文件：info 级别，正常跳过
    - 读取失败/上传失败：error 级别，但不抛异常，skill 主流程继续
      （产物上传是异步增强，URL 仍会构造但 bridge 端会 404，已知降级）
    """
    # AF_ARTIFACT_STORAGE_URL 默认指向 bridge 自身地址（http://localhost:8199)
    storage_url = os.environ.get("AF_ARTIFACT_STORAGE_URL", "http://localhost:8199")
    _t0 = time.monotonic()
    agent_name = os.path.basename(_pkg_dir().rstrip(os.sep))
    entries: list[dict] = []
    if not storage_url:
        logger.info("artifact.upload.skip: AF_ARTIFACT_STORAGE_URL not set")
        _ms = (time.monotonic() - _t0) * 1000
        print(f"[timing] artifact.upload_to_storage duration_ms={_ms:.1f}", file=sys.stderr)
        _emit_artifact_storage_result(
            "storage_disabled",
            agent_name=agent_name,
            session_id="",
            entries=[
                {"source_path": str(path), "status": "storage_disabled"}
                for path in artifact_paths
            ],
        )
        return
    # artifact 路由键用 {user_id}_{client_id}（artifact-storage.md §2.2 + §I-artifact-id），
    # 不是 AF_SESSION_ID（执行槽位 0/1/2，缺用户维度会导致跨用户产物撞 artifact_id）。
    session_id = _artifact_session_id()
    if not session_id:
        logger.info("artifact.upload.skip: no AF_USER_ID/AF_CLIENT_ID")
        _ms = (time.monotonic() - _t0) * 1000
        print(f"[timing] artifact.upload_to_storage duration_ms={_ms:.1f}", file=sys.stderr)
        _emit_artifact_storage_result(
            "identity_missing",
            agent_name=agent_name,
            session_id="",
            entries=[
                {"source_path": str(path), "status": "identity_missing"}
                for path in artifact_paths
            ],
        )
        return
    _sandbox_mode = os.environ.get("AF_SANDBOX_MODE", "unknown")
    files = []
    file_entry_indexes: list[int] = []
    total_bytes = 0
    for path in artifact_paths:
        entry = {"source_path": str(path)}
        entries.append(entry)
        if path.startswith(("http://", "https://")):
            logger.info("artifact.upload.skip_external_url path=%s", path)
            entry.update({"status": "external_reference", "uri": path})
            continue  # 外部 URL 不上传
        p = Path(path)
        if not p.is_file():
            logger.warning("artifact.upload.skip_missing path=%s", path)
            entry["status"] = "missing"
            continue
        try:
            data = p.read_bytes()
        except OSError as e:
            logger.error("artifact.upload.read_failed path=%s err=%s", path, e)
            entry.update({
                "status": "read_failed",
                "error_type": type(e).__name__,
                "error": str(e),
            })
            continue
        total_bytes += len(data)
        # artifact_id = {agent_name}/{session_id}/{file_path}
        # file_path 是相对 AGENT_PACKAGE_DIR 的路径
        # _ensure_artifacts_in_package 已按 session_id 分目录保存，
        # 因此 rel 形如 "outputs/{session_id}/{filename}"。
        # 上传时需要剥离 session_id 段，避免 Bridge 侧 artifact_id 重复。
        try:
            rel = p.resolve().relative_to(Path(_pkg_dir()).resolve())
            # 剥离 session_id 段：outputs/{session_id}/{filename} → outputs/{filename}
            rel_parts = rel.parts
            if len(rel_parts) > 2 and rel_parts[0] == "outputs" and rel_parts[1] == session_id:
                rel = Path(rel_parts[0]) / Path(*rel_parts[2:])
        except ValueError:
            rel = Path(p.name)  # 外部文件，用文件名（与 local_artifact_url basename 约定一致）
        entry.update({
            "status": "prepared",
            "storage_path": str(rel),
            "size_bytes": len(data),
            "sha256": hashlib.sha256(data).hexdigest(),
        })
        file_entry_indexes.append(len(entries) - 1)
        # base64 编码二进制产物，避免 utf-8 decode 损坏 PNG/PDF 等格式
        import base64
        files.append({
            "path": str(rel),
            "data_b64": base64.b64encode(data).decode("ascii"),
        })
    if not files:
        logger.info("artifact.upload.skip: no files to upload "
                      f"agent={agent_name} session={session_id} sandbox_mode={_sandbox_mode}")
        _ms = (time.monotonic() - _t0) * 1000
        print(f"[timing] artifact.upload_to_storage duration_ms={_ms:.1f}", file=sys.stderr)
        _emit_artifact_storage_result(
            "no_uploadable_files",
            agent_name=agent_name,
            session_id=session_id,
            entries=entries,
        )
        return
    import urllib.request
    body = json.dumps({"agent_name": agent_name, "session_id": session_id, "files": files}).encode("utf-8")
    upload_url = f"{storage_url.rstrip('/')}/api/artifacts/upload"
    file_names = [f.get("path", "?") for f in files]
    logger.info(
        "artifact.upload.start url=%s agent=%s session=%s "
        "sandbox_mode=%s files=%d total_bytes=%d names=%s",
        upload_url, agent_name, session_id,
        _sandbox_mode, len(files), total_bytes, file_names,
    )
    storage_status = "upload_failed"
    uploaded_count = 0
    returned_artifact_ids: list[str] = []
    upload_error_type = ""
    upload_error = ""
    try:
        req = urllib.request.Request(
            upload_url,
            data=body,
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        with urllib.request.urlopen(req, timeout=10) as resp:
            resp_body = json.loads(resp.read().decode("utf-8"))
            raw_ids = resp_body.get("artifact_ids", [])
            if not isinstance(raw_ids, list):
                raw_ids = []
            returned_artifact_ids = [
                str(artifact_id)
                for artifact_id in raw_ids
                if isinstance(artifact_id, str) and artifact_id
            ]
            for ordinal, entry_index in enumerate(file_entry_indexes):
                artifact_entry = entries[entry_index]
                if ordinal < len(returned_artifact_ids):
                    artifact_entry.update({
                        "status": "uploaded",
                        "artifact_id": returned_artifact_ids[ordinal],
                    })
                    uploaded_count += 1
                else:
                    artifact_entry["status"] = "upload_response_missing_id"
            storage_status = "uploaded" if uploaded_count == len(files) else "partial"
            logger.info(
                "artifact.upload.done url=%s agent=%s session=%s "
                "sandbox_mode=%s count=%d ids=%s",
                upload_url, agent_name, session_id,
                _sandbox_mode, len(returned_artifact_ids), returned_artifact_ids,
            )
    except Exception as e:
        storage_status = "upload_failed"
        upload_error_type = type(e).__name__
        upload_error = str(e)
        for entry_index in file_entry_indexes:
            if entries[entry_index].get("status") == "prepared":
                entries[entry_index]["status"] = "upload_failed"
        # 记录完整请求上下文，避免 e2b 等远端模式无法定位
        logger.error(
            "artifact.upload.failed url=%s agent=%s session=%s "
            "sandbox_mode=%s file_count=%d total_bytes=%d names=%s err=%s",
            upload_url, agent_name, session_id,
            _sandbox_mode, len(files), total_bytes, file_names, e,
        )
    finally:
        _ms = (time.monotonic() - _t0) * 1000
        print(f"[timing] artifact.upload_to_storage duration_ms={_ms:.1f}", file=sys.stderr)
        _emit_artifact_storage_result(
            storage_status,
            agent_name=agent_name,
            session_id=session_id,
            entries=entries,
            attempted_count=len(files),
            uploaded_count=uploaded_count,
            returned_artifact_ids=returned_artifact_ids,
            error_type=upload_error_type,
            error=upload_error,
        )
