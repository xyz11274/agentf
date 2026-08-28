"""Coordinator 公共逻辑 — v1 / v2 共享的纯函数与产物处理。

霰弹式收敛(2026-08-14)：v1(coordinator_decide.py) 与 v2(coordinator_agent_loop.py)
此前各自维护一份 parse_agent_md / e2e_response / 产物 URL 重写 / llm_error 兜底 /
session 保存 / LLM extra_body / 空响应检测——同类逻辑 2+ 处独立实现,
改一处漏另一处(v2 的 parse_agent_md 曾缺 env 展开、v2 的 e2e_response key 曾与 v1 不同,
都是霰弹式修改的实证)。本模块收敛为单一真相源, v1/v2 均从这里 import。

模块级路径常量由 v1/v2 的 execute() 开头调用 ``bind_package_paths()`` 同步
(与 v2 对 skill_executor 的同步方式一致——entry file 只注入各自模块的
AGENT_PACKAGE_DIR, 公共模块需要显式绑定)。
"""

from __future__ import annotations

import copy
import json
import logging
import os
import re
import sys
import time
from pathlib import Path
from urllib.parse import quote

import yaml

# ── Add nodes/ to sys.path for local imports (before .venv activation) ──
# 与 coordinator_decide.py 同款:prompts 包位于 agentflow_runtime/ 下,
# `from prompts import ...` 依赖本目录在 sys.path(entry file 注入前兜底)。
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from agentflow_runtime.common.runtime_context import (
    artifact_session_id as _artifact_session_id,
    dialogue_id as _dialogue_id,
)
from prompts import (
    ARTIFACT_DOWNLOAD_TEMPLATE,
    ARTIFACT_HTML_TEMPLATE,
    ARTIFACT_IMAGE_TEMPLATE,
    ARTIFACTS_SECTION_HEADER,
    LLM_ERROR_NO_ARTIFACTS,
    LLM_ERROR_WITH_ARTIFACTS,
)

logger = logging.getLogger("agentflow_runtime.coordinator_common")

# ── 路径常量（由 v1/v2 execute() 开头 bind_package_paths 同步） ──
AGENT_PACKAGE_DIR: str = os.environ.get("AF_AGENT_PACKAGE_DIR", os.getcwd())
AGENT_MD_PATH: str = os.path.join(AGENT_PACKAGE_DIR, "agent.md")
NODES_DIR: str = os.path.dirname(os.path.abspath(__file__))


def bind_package_paths(package_dir: str, agent_md_path: str, nodes_dir: str) -> None:
    """把 v1/v2 entry file 注入的包路径同步到公共模块（霰弹式收敛的绑定点）。

    v1/v2 的 entry file(nodes/coordinator_decide.py / nodes/coordinator_decide_v2.py)
    只注入各自模块的 AGENT_PACKAGE_DIR/AGENT_MD_PATH/NODES_DIR;公共模块若不同步,
    产物 URL 会落到 pip 包路径(与 v2 对 skill_executor 的同步同款模式)。
    """
    global AGENT_PACKAGE_DIR, AGENT_MD_PATH, NODES_DIR
    AGENT_PACKAGE_DIR = package_dir
    AGENT_MD_PATH = agent_md_path
    NODES_DIR = nodes_dir


# ── agent.md frontmatter 解析（v1 完整版：env 展开 + 异常兜底） ──

RE_ENV_VAR = re.compile(r"\$\{(\w+)\}")


def expand_env(value):
    """递归展开字符串中的 ${ENV_VAR} 占位符（v1 语义, v2 曾缺失）。"""
    if isinstance(value, str):
        return RE_ENV_VAR.sub(lambda m: os.environ.get(m.group(1), m.group(0)), value)
    if isinstance(value, dict):
        return {k: expand_env(v) for k, v in value.items()}
    if isinstance(value, list):
        return [expand_env(v) for v in value]
    return value


def parse_agent_md(path: str) -> tuple[dict, str]:
    """解析 agent.md frontmatter(YAML) + 正文。

    统一 v1 完整版语义:文件缺失返回空、frontmatter 做 ${ENV_VAR} 展开、
    解析异常兜底为 {} 并打印 WARN(v2 原简化版缺 env 展开与异常兜底)。
    """
    if not os.path.isfile(path):
        return {}, ""
    with open(path, encoding="utf-8") as f:
        raw = f.read()
    parts = raw.split("---", 2)
    if len(parts) < 3:
        return {}, raw.strip()
    try:
        frontmatter = yaml.safe_load(parts[1]) or {}
        frontmatter = expand_env(frontmatter)
    except Exception:
        frontmatter = {}
        print("[flow] WARN: agent.md frontmatter parse failed", file=sys.stderr)
    body = parts[2].strip()
    return frontmatter, body


# ── e2e 响应旁路（统一 v1 的 `_e2e_response` global key） ──

def put_e2e_response(databus, response_text: str, *, log_timing: bool = False) -> None:
    """把 final response text 写到 DataBus key `_e2e_response`——给外部 e2e 测试
    daemon 直连 JSON-RPC 读的旁路钩子。

    性质说明(这是多处 copy-paste 霰弹式收敛的公共抽象):
      - 不是真业务路径——真业务路径是 session_router->enqueue_response_message()。
      - 失败只 WARN 不抛——dialogue cleanup 后 DataBus 拒写已 cleanup key 是合理的。
      - WARN 留 stderr:让 skill_stderr.<pid>.log 能看到失败,但不污染真路径。
    统一 key:v1 用 `_e2e_response`(global namespace),v2 曾用 dialogue-scoped
    `e2e_response:{sid}:{did}`——e2e 测试读 `_e2e_response`,收敛到 v1。
    """
    if not databus.is_connected():
        return
    try:
        if log_timing:
            _t7 = time.time()
            databus._client.raw().put("_e2e_response", response_text)
            print(f"[timing] e2e_put={time.time()-_t7:.1f}s", file=sys.stderr)
        else:
            databus._client.raw().put("_e2e_response", response_text)
    except Exception as exc:
        print(f"[flow] WARN: e2e_response put failed: {exc}", file=sys.stderr)


# ── LLM extra_body 构造（v1/v2 完全相同） ──

def build_llm_extra_body() -> dict:
    """构造 call_llm 的 extra_body 参数（v1/v2 曾各写一份完全相同的 dict）。"""
    from agentflow_runtime.common.agent_config import AgentConfig
    return {
        "temperature": AgentConfig.llm_temperature(),
        "top_p": AgentConfig.llm_top_p(),
        "frequency_penalty": AgentConfig.llm_frequency_penalty(),
        "presence_penalty": AgentConfig.llm_presence_penalty(),
        "top_k": AgentConfig.llm_top_k() or None,
        "stop": AgentConfig.llm_stop() or None,
    }


# ── 空响应检测（v1 语义） ──

def is_empty_llm_response(raw: dict) -> bool:
    """检测没有用户可见内容、也没有工具调用的 LLM 响应。"""
    if not isinstance(raw, dict):
        return True
    choices = raw.get("choices")
    if not isinstance(choices, list) or not choices:
        return True
    choice = choices[0]
    if not isinstance(choice, dict):
        return True
    msg = choice.get("message")
    if not isinstance(msg, dict):
        return True
    content = msg.get("content", "")
    has_content = (
        bool(content.strip()) if isinstance(content, str) else bool(content)
    )
    return not has_content and not msg.get("tool_calls")


# ── session 保存（v2 曾独立实现, v1 曾内联 3 处） ──

def save_session_messages(recorder, messages: list[dict]) -> None:
    """保存运行态快照，并以 append-only 事件记录当时的有效消息。"""
    messages_to_save = messages[1:]
    try:
        from agentflow_runtime import events as hook_events
        from agentflow_runtime.hooks import trigger as hook_trigger

        hook_trigger(hook_events.COORDINATOR_MESSAGES_SNAPSHOT, {
            "dialogue_id": _dialogue_id(),
            "messages": copy.deepcopy(messages_to_save),
            "message_count": len(messages_to_save),
        })
    except Exception as exc:
        print(f"[flow] WARN: messages snapshot hook failed: {exc}", file=sys.stderr)
    if not recorder.is_connected:
        return
    _t_save = time.time()
    recorder.write_session_messages(messages_to_save)
    print(f"[timing] session_save={time.time()-_t_save:.1f}s", file=sys.stderr)


# ── 产物 URL 重写 + 产物块追加（v1 权威实现, v2 曾 import v1 / 简化版缺失） ──

_VERSION_SUFFIX = re.compile(r"-(\d+)$")


def _dedup_versioned_artifacts(paths: list[str]) -> list[str]:
    """Keep only the highest-numbered variant when filenames share a ``-N`` stem."""
    best: dict[str, tuple[int, str]] = {}
    for path in paths:
        stem = Path(path).stem
        match = _VERSION_SUFFIX.search(stem)
        base = stem[: match.start()] if match else stem
        num = int(match.group(1)) if match else 0
        prev = best.get(base)
        if prev is None or num > prev[0]:
            best[base] = (num, path)
    return [path for _, path in best.values()]


def _existing_artifact_url(path: str) -> str | None:
    """Resolve a path to a servable URL, returning None if the file is missing."""
    if path.startswith(("http://", "https://")):
        return path
    if not os.path.isfile(path):
        print(f"[flow] WARN: artifact not found on disk, skip URL: {path}", file=sys.stderr)
        return None
    return local_artifact_url(path)


def local_artifact_url(path: str, package_dir: str | None = None) -> str:
    """Map a package-local path to a file-storage artifact URL.

    Returns ``/api/artifacts/{artifact_id}`` where artifact_id is
    ``{agent_name}/{session_id}/{relative_path}``.
    session_id 段走 _artifact_session_id()({user_id}_{client_id}),
    与 skill_executor._upload_artifacts_to_storage 落盘目录段单一真相来源。
    """
    if package_dir is None:
        package_dir = AGENT_PACKAGE_DIR
    agent_name = os.path.basename(package_dir.rstrip(os.sep)) or "agent"
    relative = os.path.relpath(os.path.realpath(path), os.path.realpath(package_dir))
    session_id = _artifact_session_id()
    storage_url = os.environ.get("AF_ARTIFACT_STORAGE_URL", "")
    if session_id and storage_url:
        parts = relative.split("/")
        if len(parts) > 2 and parts[0] == "outputs" and parts[1] == session_id:
            relative = "/".join([parts[0]] + parts[2:])
    if relative.startswith(".."):
        relative = os.path.basename(path)

    _format = "session_id" if session_id and storage_url else "legacy"
    _sandbox_mode = os.environ.get("AF_SANDBOX_MODE", "unknown")
    if session_id and storage_url:
        url = f"/api/artifacts/{agent_name}/{session_id}/{quote(relative, safe='/')}"
    else:
        url = f"/api/artifacts/{agent_name}/files/{quote(relative, safe='/')}"

    print(
        f"[flow] artifact.local_artifact_url url={url} format={_format} "
        f"agent={agent_name} session_id={session_id or '(unset)'} "
        f"storage_url={'set' if storage_url else 'unset'} "
        f"sandbox_mode={_sandbox_mode} path={path} relative={relative}",
        file=sys.stderr,
    )
    return url


def rewrite_local_artifact_links(content: str, artifacts: list[str]) -> str:
    """Rewrite local Markdown and HTML references using collected artifacts."""
    local_artifacts = {
        os.path.realpath(path): path
        for path in artifacts
        if not path.startswith(("http://", "https://")) and os.path.isfile(path)
    }
    resolved_count = 0
    unresolved_refs: list[str] = []

    def resolve_url(reference: str) -> str | None:
        nonlocal resolved_count
        reference = reference.strip()
        if not reference or reference.startswith(("http://", "https://", "data:", "#")):
            if reference:
                unresolved_refs.append(f"{reference}(skip:external)")
            return None
        candidate = reference if os.path.isabs(reference) else os.path.join(
            AGENT_PACKAGE_DIR, reference
        )
        real_candidate = os.path.realpath(candidate)
        if real_candidate not in local_artifacts:
            base = os.path.basename(reference)
            if base:
                matches = [p for p in local_artifacts.values() if os.path.basename(p) == base]
                if len(matches) == 1:
                    resolved_count += 1
                    return local_artifact_url(matches[0])
            unresolved_refs.append(f"{reference}(not_in_artifacts)")
            return None
        resolved_count += 1
        return local_artifact_url(local_artifacts[real_candidate])

    markdown_pattern = re.compile(r"(!?\[[^\]]*\]\()([^)\s]+)(\))", re.I)

    def replace_markdown(match: re.Match) -> str:
        url = resolve_url(match.group(2))
        if not url:
            return match.group(0)
        return f"{match.group(1)}{url}{match.group(3)}"

    content = markdown_pattern.sub(replace_markdown, content)
    html_pattern = re.compile(
        r"(?P<prefix>\b(?:src|href|poster)\s*=\s*)(?P<quote>[\"'])(?P<path>[^\"']+)(?P=quote)",
        re.I,
    )

    def replace_html(match: re.Match) -> str:
        url = resolve_url(match.group("path"))
        if not url:
            return match.group(0)
        quote_char = match.group("quote")
        return f"{match.group('prefix')}{quote_char}{url}{quote_char}"

    content = html_pattern.sub(replace_html, content)

    print(
        f"[flow] artifact.rewrite_links total_artifacts={len(local_artifacts)} "
        f"resolved={resolved_count} unresolved={len(unresolved_refs)}",
        file=sys.stderr,
    )
    if unresolved_refs:
        print(
            "[flow] artifact.rewrite_links_unresolved "
            + json.dumps(unresolved_refs[:20], ensure_ascii=False),
            file=sys.stderr,
        )
    return content


def rewrite_local_image_links(content: str) -> str:
    """Backward-compatible absolute Markdown image rewriting helper."""
    absolute_images = re.findall(
        r"!\[[^\]]*\]\((/[^)]+\.(?:png|jpe?g|webp|gif))\)", content, re.I
    )
    return rewrite_local_artifact_links(content, absolute_images)


def append_artifacts_markdown(content: str, artifacts_json: str) -> str:
    """Append browser-accessible Markdown for generated artifacts (images, HTML, downloads)."""
    try:
        artifacts = json.loads(artifacts_json) if artifacts_json else []
    except (json.JSONDecodeError, TypeError):
        artifacts = []
    items = [item for item in artifacts if isinstance(item, str)]
    legacy_absolute_images = re.findall(
        r"!\[[^\]]*\]\((/[^)]+\.(?:png|jpe?g|webp|gif))\)", content, re.I
    )
    content = rewrite_local_artifact_links(
        content, list(dict.fromkeys(items + legacy_absolute_images))
    )
    if not items:
        return content

    _image_suffixes = {".png", ".jpg", ".jpeg", ".webp", ".gif"}
    all_images = [p for p in items if Path(p).suffix.lower() in _image_suffixes]
    images = _dedup_versioned_artifacts(all_images)
    html_reports = [p for p in items if p.lower().endswith(".html")]
    downloads = [p for p in items if p not in all_images and p not in html_reports]

    blocks = []
    skipped_already_in_content: list[str] = []

    def _is_resolved_ref(ref: str) -> bool:
        return ref.startswith(("/api/artifacts/", "http://", "https://", "data:"))

    existing_content_keys: set[str] = set()
    for _m in re.finditer(r"!?\[[^\]]*\]\(([^)\s]+)\)", content, re.I):
        _ref = _m.group(1).split("?")[0].split("#")[0]
        if not _is_resolved_ref(_ref):
            continue
        _name = Path(_ref).name
        if _name:
            existing_content_keys.add(_name)
    for _m in re.finditer(r"<img[^>]*\bsrc\s*=\s*[\"']([^\"']+)[\"']", content, re.I):
        _ref = _m.group(1).split("?")[0].split("#")[0]
        if not _is_resolved_ref(_ref):
            continue
        _name = Path(_ref).name
        if _name:
            existing_content_keys.add(_name)

    def _should_append(path: str, url: str) -> bool:
        key = Path(path).name
        if key in existing_content_keys:
            return False
        existing_content_keys.add(key)
        return True

    if images:
        for index, path in enumerate(images, 1):
            url = _existing_artifact_url(path)
            if url and _should_append(path, url):
                blocks.append(ARTIFACT_IMAGE_TEMPLATE.format(index=index, url=url))
            elif url:
                skipped_already_in_content.append(path)
    if html_reports:
        for index, path in enumerate(html_reports, 1):
            url = _existing_artifact_url(path)
            if url and _should_append(path, url):
                blocks.append(ARTIFACT_HTML_TEMPLATE.format(index=index, url=url))
            elif url:
                skipped_already_in_content.append(path)
    if downloads:
        for index, path in enumerate(downloads, 1):
            url = _existing_artifact_url(path)
            if url and _should_append(path, url):
                blocks.append(ARTIFACT_DOWNLOAD_TEMPLATE.format(index=index, url=url))
            elif url:
                skipped_already_in_content.append(path)

    print(
        f"[flow] artifact.markdown input_items={len(items)} "
        f"images={len(images)} html={len(html_reports)} downloads={len(downloads)} "
        f"blocks={len(blocks)} skipped_in_content={len(skipped_already_in_content)}",
        file=sys.stderr,
    )
    if blocks:
        block_urls = [b.split("](")[-1].rstrip(")") for b in blocks]
        print(
            "[flow] artifact.markdown_urls " + json.dumps(block_urls, ensure_ascii=False),
            file=sys.stderr,
        )
    if skipped_already_in_content:
        print(
            "[flow] artifact.markdown_skipped_in_content "
            + json.dumps(skipped_already_in_content, ensure_ascii=False),
            file=sys.stderr,
        )
    if not blocks:
        return content
    return content.rstrip() + "\n\n" + ARTIFACTS_SECTION_HEADER + "\n\n" + "\n\n".join(blocks)


# ── LLM error 兜底（v1 语义:有产物时带产物块） ──

def llm_error_response(artifacts_json: str) -> str:
    """Build an honest fallback based on artifacts actually reported."""
    try:
        artifacts = json.loads(artifacts_json) if artifacts_json else []
    except (json.JSONDecodeError, TypeError):
        artifacts = []
    has_artifacts = any(isinstance(item, str) and item for item in artifacts)
    if has_artifacts:
        return append_artifacts_markdown(LLM_ERROR_WITH_ARTIFACTS, artifacts_json)
    return LLM_ERROR_NO_ARTIFACTS
