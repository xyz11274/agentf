#!/usr/bin/env python3
"""CubeSandbox 增量快照模板创建脚本（REST snapshot 通道）。

用途：基于已有基础模板（tpl-*）构建"增量快照模板"（snap-*）——把最新 agent
package / 系统资源上传到沙箱后固化，使段2 `Sandbox.create(template=snap-*)`
资产已在 → 跳过 e2b_sync 全量上传 → 首问毫秒级（实测快照复用 0.12-0.19s）。

方案背景（2026-08-09 实测坐实，见 canonical 06-模板与快照语义.md）：
- snapshot 可作蓝图：`Sandbox.create(template="snap-*")` 成功，且固化资产在新沙箱保留
- 不需要 docker build（4.7GB 镜像 ~98s + 注册轮询），快照固化秒级
- 上传资产前提：SDK envd 通信可用——必须完整 patch 4 处 envd_api_url
  （只 patch _pty → commands.run 走 _commands 仍用 hostname URL → nginx SNI RST → errno=54）

流程：
1. 旧模板拉起沙箱（实测 0.17s）
2. 完整 patch 4 处 envd_api_url（与 e2b_lifecycle.py cmd_create 一致）
3. 上传 agent package / 系统资源到沙箱可写目录（$HOME/workspace，非 /opt——非 root）
4. POST /sandboxes/{id}/snapshots 固化 → 拿 snapshotID
5. 清理临时沙箱，输出快照 ID（可写入 daemon 配置 e2b_default_template）

⚠️ 资产源 = ② 固化态（agent-packages），不是 ③ workspace（examples/agent）：
快照固化的是 agent 静态资产（harness/skills 等），应从权威源
`resource/agent-packages/{agent_id}/` 取（三态模型 ②，见 agent-package-three-state.md）。
`examples/agent/{agent_id}/` 是 ③ 运行实例（含 __pycache__/记忆/动态 skill 等运行时产物），
不得作为快照资产源——否则快照含 ③ 独有内容（如 dashscope_outfit_skills），与 ② 不一致。

用法:
# 默认（基础模板取 config e2b_default_template，资产清单默认不传文件只建目录）
python3 scripts/e2b/template_snapshot_create.py

# 指定基础模板 + 快照名 + 上传单个文件（远端路径在沙箱内 $HOME/workspace 下）
python3 scripts/e2b/template_snapshot_create.py \
  --base-template tpl-cb6b9dcbf5784a1487a75971 \
  --name agentflow-warm-incr-v1 \
  --upload-file resource/agent-packages/outfit-assistant/package.json:/home/user/workspace/agent_pkg/package.json

# 上传 manifest 资产目录（递归，$HOME/workspace/ 下保留相对路径；资产源 = ② 固化态）
python3 scripts/e2b/template_snapshot_create.py \
  --manifest resource/agent-packages/outfit-assistant/manifest.jsonc \
  --local-root resource/agent-packages/outfit-assistant

环境变量/配置:
configs/test/agentflowd.json 的 sandbox 段（e2b_api_url / e2b_api_key / e2b_default_template）
E2B_API_URL / E2B_API_KEY 可覆盖

输出:
新快照 ID（snap-xxx），可写入 daemon 配置 e2b_default_template
"""
import argparse
import json
import os
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
_DEFAULT_CONFIG = ROOT / "configs" / "test" / "agentflowd.json"
_LOG_DIR = ROOT / "logs" / "tests"

# 复用 e2b_sync.py 的权威 manifest 解析（F5 单一真相来源）
_SYNC_DIR = ROOT / "src" / "sandbox"
if str(_SYNC_DIR) not in sys.path:
    sys.path.insert(0, str(_SYNC_DIR))


def _log(msg: str) -> None:
    print(f"[snapshot_create] {msg}", file=sys.stderr, flush=True)


def _load_config() -> dict:
    """读 config sandbox 段（e2b_api_url / e2b_api_key / e2b_default_template）。"""
    if not _DEFAULT_CONFIG.is_file():
        _log(f"ERROR: config not found: {_DEFAULT_CONFIG}")
        sys.exit(2)
    with open(_DEFAULT_CONFIG) as f:
        cfg = json.load(f).get("sandbox", {})
    if not cfg.get("e2b_api_url") or not cfg.get("e2b_api_key"):
        _log("ERROR: config missing e2b_api_url / e2b_api_key")
        sys.exit(2)
    return cfg


def _setup_env(cfg: dict) -> None:
    """设置 SDK 环境变量（E2B_API_URL / E2B_API_KEY / SSL_CERT_FILE）。"""
    os.environ["E2B_API_URL"] = cfg["e2b_api_url"]
    os.environ["E2B_API_KEY"] = cfg["e2b_api_key"]
    cert = cfg.get("e2b_ca_cert_path", "")
    if cert:
        p = Path(cert)
        if not p.is_absolute():
            p = ROOT / p
        if p.is_file():
            os.environ["SSL_CERT_FILE"] = str(p.resolve())
            _log(f"SSL_CERT_FILE -> {p.resolve()}")


def _apply_ssl_patch() -> None:
    """SSL check_hostname=False + CERT_NONE（与 e2b_lifecycle.py 一致）。"""
    import ssl

    if getattr(ssl, "_agentflow_patched", False):
        return
    _orig = ssl.create_default_context

    def _patched(*args, **kwargs):
        ctx = _orig(*args, **kwargs)
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
        return ctx

    ssl.create_default_context = _patched
    ssl._agentflow_patched = True  # type: ignore[attr-defined]


def _patch_envd_url_full(sb) -> list:
    """完整 patch 4 处 envd_api_url（根因修复，§1.4 / 06 文档 §2.4）。

    只 patch _pty 会导致 commands.run/files.write 仍用 SDK 自动构造的 hostname URL
    （https://{port}-{id}.cube.app）→ nginx server_name `_` 不识别 SNI → RST（errno=54）。
    """
    path = f"https://218.245.96.148:443/sandbox/{sb.sandbox_id}/49983"
    patched = []
    try:
        sb._SandboxBase__envd_api_url = path
        patched.append("_SandboxBase__envd_api_url")
    except AttributeError:
        pass
    for attr_name in ("_filesystem", "_commands", "_pty"):
        obj = getattr(sb, attr_name, None)
        if obj is not None:
            try:
                obj._envd_api_url = path
                patched.append(attr_name)
            except AttributeError:
                pass
    return patched


def _upload_assets(sb, upload_files: list, upload_dirs: list) -> int:
    """上传资产到沙箱可写目录 $HOME/workspace。

    - upload_files: ["本地路径:远端路径", ...]（单个文件）
    - upload_dirs:  ["本地目录:远端目录", ...]（递归，保留相对路径）

    远端路径必须是用户可写目录（/home/user，非 /opt——非 root 无写权限）。
    返回上传文件数。
    """
    count = 0
    for spec in upload_files:
        local, _, remote = spec.partition(":")
        if not local or not remote:
            _log(f"WARN: skip invalid --upload-file spec: {spec}")
            continue
        lp = Path(local)
        if not lp.is_file():
            _log(f"WARN: local file not found: {lp}")
            continue
        # 先建远端目录
        sb.commands.run(f"mkdir -p {remote.rsplit('/', 1)[0]}", timeout=15)
        # 直接传原始字节（与 e2b_sync.py 一致）——base64 编码会写坏文件内容
        sb.files.write(remote, lp.read_bytes())
        _log(f"uploaded file: {lp.name} -> {remote}")
        count += 1

    for spec in upload_dirs:
        local, _, remote = spec.partition(":")
        if not local or not remote:
            _log(f"WARN: skip invalid --upload-dir spec: {spec}")
            continue
        lp = Path(local)
        if not lp.is_dir():
            _log(f"WARN: local dir not found: {lp}")
            continue
        for f in sorted(lp.rglob("*")):
            if not f.is_file():
                continue
            rel = f.relative_to(lp)
            rp = f"{remote}/{rel}"
            sb.commands.run(f"mkdir -p {rp.rsplit('/', 1)[0]}", timeout=15)
            sb.files.write(rp, f.read_bytes())
            count += 1
        _log(f"uploaded dir: {local} -> {remote} ({count} files total)")

    return count


def _upload_manifest(sb, manifest_path: str, local_root: str) -> int:
    """按 manifest.jsonc includes 全量上传资产到沙箱 $HOME/workspace。

    复用 e2b_sync.py 的 _load_manifest_whitelist（F5 单一真相来源）：
    解析 includes glob → 展开相对路径白名单（excludes 剔除）→ 逐文件上传。
    远端目标 = $HOME/workspace/<rel_posix>（用户可写目录，非 /opt——非 root）。
    返回上传文件数。
    """
    try:
        from e2b_sync import _load_manifest_whitelist
    except ImportError as e:
        _log(f"ERROR: 无法 import e2b_sync._load_manifest_whitelist: {e}")
        return -1

    local_root_p = Path(local_root).resolve()
    if not local_root_p.is_dir():
        _log(f"ERROR: local_root not a dir: {local_root_p}")
        return -1

    whitelist = _load_manifest_whitelist(manifest_path, local_root_p)
    if whitelist is None:
        _log(f"ERROR: manifest whitelist is None (manifest={manifest_path})")
        return -1

    # 远端根：$HOME/workspace（与 e2b_sync 的 workspace 语义对齐）
    r = sb.commands.run("echo $HOME/workspace", timeout=15)
    remote_root = (r.stdout or "/home/user/workspace").strip()
    sb.commands.run(f"mkdir -p {remote_root}", timeout=15)

    count = 0
    for rel in sorted(whitelist):
        lp = local_root_p / rel
        if not lp.is_file():
            continue
        rp = f"{remote_root}/{rel}"
        sb.commands.run(f"mkdir -p {rp.rsplit('/', 1)[0]}", timeout=15)
        # 直接传原始字节（与 e2b_sync.py 一致）——base64 编码会写坏文件内容
        sb.files.write(rp, lp.read_bytes())
        count += 1
        if count % 20 == 0:
            _log(f"  ... uploaded {count} files")

    return count


def _compute_manifest_hash(manifest_path: str, local_root: str) -> tuple[str, int]:
    """计算 manifest includes 全量内容 hash（复用 e2b_sync 权威解析）。

    返回 (hash8, 文件数)。hash = 逐文件 sha256（含相对路径 + 内容，排序确定）。
    设计 sandbox-warm-template-oci.md §2.6：快照命名 snap-<agent_id>-<version>-<hash8>，
    version/hash 任一变化 → 重建快照。
    """
    import hashlib

    try:
        from e2b_sync import _load_manifest_whitelist
    except ImportError as e:
        _log(f"ERROR: 无法 import e2b_sync._load_manifest_whitelist: {e}")
        return "", -1

    local_root_p = Path(local_root).resolve()
    if not local_root_p.is_dir():
        _log(f"ERROR: local_root not a dir: {local_root_p}")
        return "", -1

    whitelist = _load_manifest_whitelist(manifest_path, local_root_p)
    if whitelist is None:
        _log(f"ERROR: manifest whitelist is None (manifest={manifest_path})")
        return "", -1

    h = hashlib.sha256()
    count = 0
    for rel in sorted(whitelist):
        lp = local_root_p / rel
        if not lp.is_file():
            continue
        content = lp.read_bytes()
        h.update(rel.encode())
        h.update(b"\x00")
        h.update(content)
        count += 1
    return h.hexdigest()[:8], count


def _create_snapshot(cfg: dict, sandbox_id: str, name: str, description: str) -> str:
    """POST /sandboxes/{id}/snapshots 固化，返回 snapshotID。"""
    api_url = cfg["e2b_api_url"].rstrip("/")
    body = json.dumps({"name": name, "description": description}).encode()
    req = urllib.request.Request(
        f"{api_url}/sandboxes/{sandbox_id}/snapshots",
        data=body,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=60) as resp:
            doc = json.loads(resp.read().decode(errors="replace"))
            _log(f"snapshot POST {resp.status}: {json.dumps(doc, ensure_ascii=False)[:300]}")
    except urllib.error.HTTPError as e:
        _log(f"ERROR: snapshot HTTP {e.code}: {e.read().decode(errors='replace')[:300]}")
        return ""
    except Exception as e:
        _log(f"ERROR: snapshot err: {e}")
        return ""

    snap_id = (doc.get("snapshotID") or doc.get("snapshotId")
               or doc.get("templateID") or doc.get("templateId") or doc.get("id") or "")
    if not snap_id:
        _log("ERROR: snapshot response missing snapshotID")
        return ""
    return snap_id


def main() -> int:
    ap = argparse.ArgumentParser(description="CubeSandbox 增量快照模板创建（REST snapshot 通道）")
    ap.add_argument("--base-template", default="", help="基础模板（默认取 config e2b_default_template）")
    ap.add_argument("--name", default="agentflow-warm-incr", help="快照名（建议带版本号）")
    ap.add_argument("--description", default="incremental warm template", help="快照描述")
    ap.add_argument("--upload-file", action="append", default=[],
                    help="上传单个文件：本地路径:远端路径（可多次）")
    ap.add_argument("--upload-dir", action="append", default=[],
                    help="上传目录（递归）：本地目录:远端目录（可多次）")
    ap.add_argument("--manifest", default="",
                    help="manifest.jsonc 路径（agent 包根，includes 全量上传到 $HOME/workspace）")
    ap.add_argument("--local-root", default="",
                    help="manifest includes glob 的本地根目录（默认取 manifest 所在目录）")
    ap.add_argument("--keep-sandbox", action="store_true",
                    help="保留临时沙箱不清理（调试用）")
    # hash 子命令参数：计算 manifest includes 内容 hash（供 C++ preload 做 version/hash 检测）
    ap.add_argument("--hash-manifest", default="",
                    help="hash 模式：计算 manifest includes 内容 hash8（供 C++ preload 检测）")
    ap.add_argument("--hash-local-root", default="",
                    help="hash 模式：local root（默认取 --hash-manifest 所在目录）")
    args = ap.parse_args()

    # hash 子命令：计算 manifest includes 内容 hash（供 C++ preload 做 version/hash 检测）
    # 用法：template_snapshot_create.py --hash-manifest <path> [--hash-local-root <dir>]
    if args.hash_manifest:
        m = Path(args.hash_manifest)
        lr = args.hash_local_root or str(m.parent)
        h8, count = _compute_manifest_hash(str(m), lr)
        if count < 0:
            _log("ERROR: hash 计算失败")
            return 1
        print(h8)
        _log(f"manifest hash8={h8} files={count}")
        return 0

    cfg = _load_config()
    _setup_env(cfg)
    _apply_ssl_patch()

    base_template = args.base_template or cfg.get("e2b_default_template", "")
    if not base_template:
        _log("ERROR: 未指定 --base-template 且 config e2b_default_template 为空")
        return 1

    _log(f"base_template={base_template} name={args.name} uploads={len(args.upload_file) + len(args.upload_dir)}")

    from e2b_code_interpreter import Sandbox

    # 步骤 1：旧模板拉起沙箱
    t0 = time.monotonic()
    try:
        sb = Sandbox.create(template=base_template, timeout=180)
    except Exception as e:
        _log(f"ERROR: Sandbox.create failed: {e}")
        return 1
    create_s = time.monotonic() - t0
    sandbox_id = sb.sandbox_id
    _log(f"[1/5] sandbox ready: {sandbox_id} (create={create_s:.2f}s)")

    # 步骤 2：完整 patch 4 处 envd_api_url
    patched = _patch_envd_url_full(sb)
    _log(f"[2/5] envd_api_url patched: {patched}")
    time.sleep(3)

    # 步骤 3：上传资产（--manifest 优先全量，其次 --upload-file/--upload-dir）
    if args.manifest:
        manifest_p = Path(args.manifest)
        local_root = args.local_root or str(manifest_p.parent)
        n = _upload_manifest(sb, args.manifest, local_root)
        if n < 0:
            _log("ERROR: manifest 上传失败")
            if not args.keep_sandbox:
                sb.kill()
            return 1
        _log(f"[3/5] manifest uploaded {n} files (includes 全量 → $HOME/workspace)")
    elif args.upload_file or args.upload_dir:
        n = _upload_assets(sb, args.upload_file, args.upload_dir)
        _log(f"[3/5] uploaded {n} files")
    else:
        _log("[3/5] 无上传资产（--manifest / --upload-file / --upload-dir 为空），仅固化基础模板状态")

    # 步骤 4：固化快照
    snap_id = _create_snapshot(cfg, sandbox_id, args.name, args.description)
    if not snap_id:
        _log("ERROR: 快照创建失败")
        if not args.keep_sandbox:
            sb.kill()
        return 2
    _log(f"[4/5] snapshot created: {snap_id}")

    # 步骤 5：清理 + 输出
    if not args.keep_sandbox:
        sb.kill()
        _log("[5/5] temp sandbox killed")
    else:
        _log(f"[5/5] keep-sandbox: 临时沙箱保留 {sandbox_id}")

    print(snap_id)
    _log(f"快照模板就绪: {snap_id} → 可写入 daemon 配置 e2b_default_template")
    return 0


if __name__ == "__main__":
    sys.exit(main())
