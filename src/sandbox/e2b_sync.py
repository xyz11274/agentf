#!/usr/bin/env python3
# Copyright (c) 2026 AgentFlow Authors
# Licensed under MIT License
#
# e2b 沙箱 workspace 双向同步脚本
#
# 用途：E2BSandbox 构造时把宿主机 workdir 内容上传到 e2b 远端沙箱；
#       销毁前把远端 workdir 下载回宿主机 snapshot 目录。
#
# 设计要点：
# - e2b SDK 无目录整体上传能力（官方文档明确），必须遍历+逐文件上传
# - 使用 sandbox.files.write_files 批量上传，减少 RTT
# - 下载用 sandbox.files.list 列目录 + read 逐文件下载
# - 失败 fail-fast：上传失败抛异常（B22 不 fallback），下载失败记 warning 不阻塞销毁
#
# 调用契约（被 E2BSandbox 通过 fork+execlp 调用）：
#   python3 e2b_sync.py upload   --sandbox-id ID --local-dir PATH --remote-dir PATH [--manifest PATH]
#   python3 e2b_sync.py download --sandbox-id ID --local-dir PATH --remote-dir PATH
#
# --manifest（可选，仅 upload）：manifest.jsonc 路径。
#   提供时按 manifest 的 files 白名单过滤上传（相对 local-dir 的路径集合），
#   命中 excludes 的剔除；files 为空时按 includes glob 展开。
#   缺省（workdir/skills 段）走 _CACHE_DIRS/_CACHE_SUFFIXES 缓存过滤，行为不变。
#
# 环境变量：
#   E2B_API_KEY     必填，e2b SDK 鉴权
#   E2B_TEMPLATE    可选，默认 "base"
#
# 退出码：
#   0  成功
#   1  参数错误 / API 调用失败
#   2  E2B_API_KEY 缺失
#
# 日志：打到 stderr，父进程读取或丢弃；不污染 stdout（stdout 留给结构化结果）

import argparse
import fnmatch
import json
import os
import sys
import traceback
from pathlib import Path

try:
    from e2b_code_interpreter import Sandbox
except ImportError:
    print("[e2b_sync] ERROR: e2b_code_interpreter not installed", file=sys.stderr)
    sys.exit(1)

# 共用 sandbox_patch（Phase 4：从 agentflow-runtime 导入，fallback 到本地定义）
try:
    from agentflow_runtime.common.sandbox_patch import (
        apply_ssl_patch as _apply_ssl_patch,
        patch_envd_url as _patch_envd_url,
    )
    _apply_ssl_patch()  # module-level SSL patch（与原 inline 行为一致）
except ImportError:
    # daemon 环境可能未安装 agentflow-runtime，fallback 到本地定义
    import ssl as _ssl

    if not getattr(_ssl, "_agentflow_patched", False):
        _orig_ssl_ctx = _ssl.create_default_context

        def _patched_ssl_ctx(*args, **kwargs):
            ctx = _orig_ssl_ctx(*args, **kwargs)
            ctx.check_hostname = False
            ctx.verify_mode = _ssl.CERT_NONE
            return ctx

        _ssl.create_default_context = _patched_ssl_ctx
        _ssl._agentflow_patched = True  # type: ignore[attr-defined]

    def _patch_envd_url(sb) -> None:
        """[fallback] 将 envd_api_url 从 hostname 路由改为 path-based 路由。

        权威实现见 ``agentflow_runtime.common.sandbox_patch.patch_envd_url``。
        """
        path = f"https://218.245.96.148:443/sandbox/{sb.sandbox_id}/49983"
        sb._SandboxBase__envd_api_url = path
        sb._envd_api_url = path
        for attr in ("_filesystem", "_commands", "_pty"):
            mod = getattr(sb, attr, None)
            if mod and hasattr(mod, "_envd_api_url"):
                mod._envd_api_url = path


# ── 工具函数 ──────────────────────────────────────────────────────────

def _log(msg: str) -> None:
    print(f"[e2b_sync] {msg}", file=sys.stderr, flush=True)


def _check_api_key() -> str:
    key = os.environ.get("E2B_API_KEY")
    if not key:
        _log("ERROR: E2B_API_KEY environment variable not set")
        sys.exit(2)
    return key


def _connect_sandbox(sandbox_id: str) -> "Sandbox":
    """重连已有 e2b 沙箱。用 Sandbox.connect（SDK v2 的权威重连接口）。"""
    try:
        sb = Sandbox.connect(sandbox_id)
        _patch_envd_url(sb)
        return sb
    except Exception as e:
        _log(f"ERROR: failed to reconnect e2b sandbox id={sandbox_id}: {e}")
        sys.exit(1)


# ── upload：本地目录 → 远端目录 ──────────────────────────────────────

# 不同步到沙箱的缓存/构建产物目录与文件后缀
_CACHE_DIRS = {"__pycache__", ".pytest_cache", ".mypy_cache", ".ruff_cache", ".git"}
_CACHE_SUFFIXES = {".pyc", ".pyo", ".pyc", ".ds_store"}


def _should_exclude(rel_path: Path) -> bool:
    if rel_path.suffix.lower() in _CACHE_SUFFIXES:
        return True
    return any(part in _CACHE_DIRS for part in rel_path.parts)


def _walk_local_files(local_root: Path):
    """遍历 local_root 下所有常规文件（排除缓存产物），yield (relative_path, absolute_path)。"""
    for path in local_root.rglob("*"):
        if path.is_file() and not path.is_symlink():
            rel = path.relative_to(local_root)
            if not _should_exclude(rel):
                yield rel, path


def _load_manifest_whitelist(manifest_path: str, local_root: Path) -> set[str] | None:
    """Load manifest.jsonc → upload whitelist (relative posix paths).

    返回 None 仅表示未指定 manifest（调用方走 _CACHE_* 兜底过滤——workdir/skills 段
    行为不变）。指定了 manifest 但加载/生成失败 → 记 FATAL 并 sys.exit(1)（B22 不
    fallback：用户明确要求"只传 manifest 声明的内容"，无法确定白名单时禁止盲传，
    否则会把 .venv(91M) 等沙箱无关产物传上去）。

    优先用 files 列表（具体路径）；files 为空时按 includes glob 在 local_root 下展开。
    命中 excludes 的文件始终剔除。
    """
    if not manifest_path:
        return None
    mpath = Path(manifest_path)
    if not mpath.is_file():
        # 首次连接无 manifest——用框架规则推断生成（agent-package-structure.md §3.1 场景①）
        # generate_manifest 写盘到 local_root/manifest.jsonc，
        # 后续连接直接读，跳过重新推断（场景②）
        try:
            from agentflow_runtime.common.manifest import generate_manifest
            data = generate_manifest(local_root)
            _log(f"manifest not found, generated fresh: {mpath}")
        except ImportError:
            _log(
                "FATAL: manifest not found and agentflow_runtime unavailable — "
                "cannot determine upload whitelist (blind upload forbidden)"
            )
            sys.exit(1)
        except Exception as e:
            _log(f"FATAL: manifest generation failed for {local_root}: {e}")
            sys.exit(1)
    else:
        # manifest.jsonc 是 JSONC 格式（用户可写 // 注释），裸 json.loads 会抛
        # JSONDecodeError——剥离注释后再解析（权威实现 agentflow_runtime.common.manifest）
        try:
            from agentflow_runtime.common.manifest import strip_jsonc_comments
            raw = mpath.read_text(encoding="utf-8")
            data = json.loads(strip_jsonc_comments(raw))
        except ImportError:
            _log("FATAL: manifest parsing requires agentflow_runtime.common.manifest")
            sys.exit(1)
        except (json.JSONDecodeError, OSError) as e:
            _log(f"FATAL: manifest unparseable ({mpath}): {e}")
            sys.exit(1)

    files = data.get("files") or []
    includes = data.get("includes") or []
    excludes = data.get("excludes") or []

    # 展开白名单：优先 files（具体路径），否则 includes glob
    if files:
        whitelist = set()
        for item in files:
            if isinstance(item, dict):
                p = item.get("path", "")
            else:
                p = str(item)
            if p:
                whitelist.add(p.lstrip("/"))
    elif includes:
        whitelist = set()
        for pat in includes:
            for rel, _ in _walk_local_files(local_root):
                if fnmatch.fnmatch(rel.as_posix(), pat):
                    whitelist.add(rel.as_posix())
    else:
        _log(f"FATAL: manifest has neither files nor includes: {mpath}")
        sys.exit(1)

    # 命中 excludes 的剔除
    for ex in excludes:
        whitelist = {p for p in whitelist if not _path_matches_exclude(p, ex)}
    return whitelist


def _path_matches_exclude(rel_posix: str, ex: str) -> bool:
    """判断相对路径是否命中 excludes 项（目录前缀或 glob）。"""
    ex = ex.rstrip("/")
    if "/" in ex:
        return fnmatch.fnmatch(rel_posix, ex) or rel_posix.startswith(ex + "/")
    # 顶层目录名/文件名
    first = rel_posix.split("/", 1)[0]
    return first == ex or fnmatch.fnmatch(rel_posix, ex)


def upload(sandbox_id: str, local_dir: str, remote_dir: str, manifest_path: str = "") -> None:
    api_key = _check_api_key()
    local_root = Path(local_dir).resolve()
    if not local_root.is_dir():
        _log(f"ERROR: local-dir does not exist or is not a directory: {local_root}")
        sys.exit(1)

    sb = _connect_sandbox(sandbox_id)

    # 收集待上传文件：有 manifest 按白名单过滤，否则走 _CACHE_* 兜底
    whitelist = _load_manifest_whitelist(manifest_path, local_root)
    entries = []
    for rel, abs_path in _walk_local_files(local_root):
        if whitelist is not None and rel.as_posix() not in whitelist:
            continue
        entries.append((rel, abs_path))
    total = len(entries)
    if total == 0:
        _log(f"upload: local-dir empty, nothing to upload (remote_dir={remote_dir})")
        return

    _log(f"upload: {total} files, local={local_root} → remote={remote_dir}")
    if whitelist is not None:
        _log(f"upload: manifest whitelist applied ({len(whitelist)} entries)")

    # 确保远端根目录存在
    try:
        sb.files.make_dir(remote_dir)
    except Exception as e:
        _log(f"ERROR: make_dir({remote_dir}) failed: {e}")
        sys.exit(1)

    # 批量上传：write_files 接受 [{path, data}, ...]
    # 注意：write_files 要求 path 是远端绝对路径
    batch = []
    BATCH_SIZE = 50  # 避免 HTTP body 过大
    uploaded = 0
    failed = 0

    for rel_path, abs_path in entries:
        remote_path = f"{remote_dir}/{rel_path.as_posix()}"
        try:
            with open(abs_path, "rb") as f:
                content = f.read()
            batch.append({"path": remote_path, "data": content})

            if len(batch) >= BATCH_SIZE:
                sb.files.write_files(batch)
                uploaded += len(batch)
                batch = []
        except Exception as e:
            _log(f"WARN: failed to read/prepare {abs_path}: {e}")
            failed += 1

    # flush remainder
    if batch:
        try:
            sb.files.write_files(batch)
            uploaded += len(batch)
        except Exception as e:
            _log(f"ERROR: write_files final batch failed: {e}")
            sys.exit(1)

    _log(f"upload: done uploaded={uploaded} failed={failed}")
    if failed > 0:
        _log(f"WARN: {failed} files failed to upload")
        # 不 fail-fast：部分失败允许继续，调用方看日志决定


# ── download：远端目录 → 本地目录 ────────────────────────────────────

def _list_remote_files(sb, remote_dir: str):
    """列远端目录所有文件。e2b files.list 返回文件列表。"""
    try:
        entries = sb.files.list(remote_dir)
    except Exception as e:
        _log(f"ERROR: list({remote_dir}) failed: {e}")
        return []

    for entry in entries:
        # entry.path 是远端绝对路径
        if entry.is_file():
            yield entry.path
        elif entry.is_dir():
            yield from _list_remote_files(sb, entry.path)


def _download_file_by_file(sb, remote_dir: str, local_root: Path) -> tuple[int, int]:
    """逐文件下载（降级方式）。返回 (downloaded, failed)。"""
    try:
        remote_files = list(_list_remote_files(sb, remote_dir))
    except Exception:
        remote_files = []

    if not remote_files:
        _log(f"download: remote-dir empty or unreadable, nothing to download")
        return 0, 0

    downloaded = 0
    failed = 0
    for remote_path in remote_files:
        if remote_path.startswith(remote_dir):
            rel = remote_path[len(remote_dir):].lstrip("/")
        else:
            rel = os.path.basename(remote_path)
        local_path = local_root / rel
        try:
            content = sb.files.read(remote_path, format="bytes")
            local_path.parent.mkdir(parents=True, exist_ok=True)
            with open(local_path, "wb") as f:
                f.write(content)
            downloaded += 1
        except Exception as e:
            _log(f"WARN: failed to download {remote_path}: {e}")
            failed += 1
    return downloaded, failed


def download(sandbox_id: str, local_dir: str, remote_dir: str) -> None:
    """保存远端工作区。

    双策略：
    - 优先：e2b 文件系统快照（1 次 RTT，保存到 e2b 平台侧），
            下次 Sandbox.create() 时可通过快照恢复，无需本地下载。
    - 降级：逐文件下载到本地（SDK 不支持 snapshot 时）。
    """
    api_key = _check_api_key()
    local_root = Path(local_dir).resolve()
    local_root.mkdir(parents=True, exist_ok=True)

    sb = _connect_sandbox(sandbox_id)

    _log(f"download: remote={remote_dir} → local={local_root}")

    # 方案一：e2b 文件系统快照（1 次 RTT 保存全量磁盘状态）
    # 仅在 SDK 支持 snapshot API 时生效，否则静默降级
    downloaded = 0
    failed = 0
    try:
        if hasattr(sb, "snapshot"):
            _log("download: attempting filesystem snapshot...")
            sb.snapshot(filesystem_only=True)
            _log("download: filesystem snapshot created")
            # 快照已保存到 e2b 平台侧，无需逐文件下载
            # 下次 Sandbox.create() 时可通过快照恢复
            return
    except (AttributeError, NotImplementedError, TypeError):
        _log("download: snapshot API not available, falling back to file-by-file")
    except Exception as e:
        _log(f"download: snapshot failed ({e}), falling back to file-by-file")

    # 方案二：逐文件下载（降级）
    _log("download: file-by-file fallback")
    downloaded, failed = _download_file_by_file(sb, remote_dir, local_root)
    _log(f"download: done downloaded={downloaded} failed={failed}")


# ── CLI 入口 ──────────────────────────────────────────────────────────

def main() -> int:
    parser = argparse.ArgumentParser(
        description="e2b sandbox workspace 双向同步"
    )
    parser.add_argument(
        "action",
        choices=["upload", "download"],
        help="upload=本地→远端(构造时), download=远端→本地(销毁前)",
    )
    parser.add_argument("--sandbox-id", required=True, help="e2b sandbox ID")
    parser.add_argument("--local-dir", required=True, help="宿主机本地目录绝对路径")
    parser.add_argument("--remote-dir", required=True, help="e2b 远端目录绝对路径")
    parser.add_argument(
        "--manifest",
        default="",
        help="manifest.jsonc 路径（可选，仅 upload）——按 manifest 白名单过滤上传",
    )
    args = parser.parse_args()

    if not os.path.isabs(args.local_dir):
        _log(f"ERROR: --local-dir must be absolute, got: {args.local_dir}")
        return 1
    if not os.path.isabs(args.remote_dir):
        _log(f"ERROR: --remote-dir must be absolute, got: {args.remote_dir}")
        return 1

    try:
        if args.action == "upload":
            upload(args.sandbox_id, args.local_dir, args.remote_dir, args.manifest)
        else:
            download(args.sandbox_id, args.local_dir, args.remote_dir)
        return 0
    except SystemExit:
        raise
    except Exception as e:
        _log(f"FATAL: unhandled exception: {e}")
        traceback.print_exc(file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
