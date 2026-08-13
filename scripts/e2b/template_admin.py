#!/usr/bin/env python3
"""CubeSandbox 模板运维工具合集 — 三个历史独立脚本的合并入口（2026-08-12 整理）。

合并来源（原 scripts/e2b/ 独立脚本）：
  template_rest_check.py     → rest-check   模板 REST 接口可用性验证
  template_snapshot_create.py → snapshot    增量快照模板创建（REST snapshot 通道）
  template_warm_create.py    → warm         预热模板创建（REST + CLI commit 双通道）

用法:
  python3 scripts/e2b/template_admin.py rest-check [--api-url URL] [--api-key KEY]
             [--agent-id my-agent] [--target-template tpl-xxx]
  python3 scripts/e2b/template_admin.py snapshot [--base-template tpl-xxx]
             [--name snap-name] [--upload-file L:R] [--upload-dir L:R]
             [--manifest manifest.jsonc] [--local-root DIR] [--keep-sandbox]
  python3 scripts/e2b/template_admin.py snapshot --hash-manifest <manifest>
             [--hash-local-root <dir>]          # hash 模式：stdout 最后一行 = hash8
  python3 scripts/e2b/template_admin.py warm [--base-template tpl-xxx]
             [--server-path PATH] [--port 8080] [--remote-dir DIR]
             [--commit-method auto|rest|cli] [--cubemastercli PATH]

环境变量/配置:
  configs/test/agentflowd.json 的 sandbox 段（e2b_api_url / e2b_api_key / e2b_default_template）
  E2B_API_URL / E2B_API_KEY 可覆盖（rest-check 默认值同 sandbox_toolkit.py）

公共样板（config 加载 / env 设置 / ssl patch / envd url patch / REST 请求）
收敛到本文件底部的工具函数。
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.request
from datetime import datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
_DEFAULT_CONFIG = ROOT / "configs" / "test" / "agentflowd.json"
_LOG_DIR = ROOT / "logs" / "tests"
_DEFAULT_API_URL = "http://218.245.96.148:30030"
_DEFAULT_API_KEY = "e2b_000000000"
_CA_CERT_PATH = str(ROOT / "resource" / "certs" / "e2b_CA.pem")
_DEFAULT_SERVER = ROOT / "docker" / "sandbox" / "http_server" / "sandbox_http_server.py"
# 内部部署 path-based 反向代理（cubeproxy），SDK 默认 hostname 路由不可达
_SANDBOX_TARGET_HOST = "https://218.245.96.148:443"
_DEFAULT_ENVD_PORT = 49983
# 复用 e2b_sync.py 的权威 manifest 解析（F5 单一真相来源）
_SYNC_DIR = ROOT / "src" / "sandbox"
if str(_SYNC_DIR) not in sys.path:
    sys.path.insert(0, str(_SYNC_DIR))

# 单文件上传请求超时（秒）：e2b SDK 默认 request_timeout=None（无读超时），
# 网络异常时请求会无限挂起（实测 daemon 快照重建卡死 9+ 分钟）。
# 显式设置读超时让上传失败快速暴露，配合 exec_cmd_argv 超时 kill 兜底。
_UPLOAD_REQUEST_TIMEOUT = 60


# ── 公共样板（原各脚本重复代码收敛点）───────────────────────────────


def _log(msg: str) -> None:
    print(f"[template_admin] {msg}", file=sys.stderr, flush=True)


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
    """SSL check_hostname=False + CERT_NONE（幂等，自建部署自签名证书）。"""
    import ssl as _ssl
    if getattr(_ssl, "_agentflow_patched", False):
        return
    _orig = _ssl.create_default_context

    def _patched(*a, **kw):
        ctx = _orig(*a, **kw)
        ctx.check_hostname = False
        ctx.verify_mode = _ssl.CERT_NONE
        return ctx

    _ssl.create_default_context = _patched
    _ssl._agentflow_patched = True  # type: ignore[attr-defined]


def _patch_envd_url(sb) -> list:
    """完整 patch 4 处 envd_api_url（根因修复，path-based 路由）。

    只 patch _pty 会导致 commands.run/files.write 仍用 SDK hostname URL
    （https://{port}-{id}.cube.app）→ nginx server_name `_` 不识别 SNI → RST（errno=54）。
    返回 patched 属性名列表。
    """
    path = f"{_SANDBOX_TARGET_HOST}/sandbox/{sb.sandbox_id}/{_DEFAULT_ENVD_PORT}"
    patched = []
    try:
        sb._SandboxBase__envd_api_url = path
        patched.append("_SandboxBase__envd_api_url")
    except AttributeError:
        pass
    try:
        sb._envd_api_url = path
        patched.append("_envd_api_url")
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


def _http(method: str, url: str, body: dict | None = None, timeout: int = 10):
    """发起 HTTP 请求，返回 (status_code, body_text)。"""
    headers = {}
    api_key = os.environ.get("E2B_API_KEY", "")
    if api_key:
        headers["Authorization"] = f"Bearer {api_key}"
    data = None
    if body is not None:
        headers["Content-Type"] = "application/json"
        data = json.dumps(body).encode("utf-8")
    req = urllib.request.Request(url, data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.status, resp.read().decode("utf-8", errors="replace")
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode("utf-8", errors="replace")
    except Exception as e:  # 网络错误（连接拒绝/超时）视为不可达
        return -1, f"network error: {e}"


# ── 子命令：rest-check（原 template_rest_check.py）───────────────────


def _check(method: str, url: str, body: dict | None = None) -> tuple[int, dict | str]:
    """执行请求并解析 JSON（失败返回原始文本）。"""
    code, text = _http(method, url, body)
    if code < 0:
        return code, text
    try:
        return code, json.loads(text)
    except json.JSONDecodeError:
        return code, text


def _expect(desc: str, ok: bool, detail: str) -> tuple[bool, str]:
    """单条断言结果。"""
    status = "PASS" if ok else "FAIL"
    print(f"  [{status}] {desc} — {detail}")
    return ok, detail


def cmd_rest_check(args: argparse.Namespace) -> int:
    """CubeSandbox 模板 REST 接口可用性验证（SC-v2 验证前置，纯 HTTP 不建沙箱）。"""
    # 环境设置（rest-check 用命令行/环境变量/默认值，不读 config 文件）
    if args.api_url:
        os.environ["E2B_API_URL"] = args.api_url
    else:
        os.environ.setdefault("E2B_API_URL", _DEFAULT_API_URL)
    if args.api_key:
        os.environ["E2B_API_KEY"] = args.api_key
    else:
        os.environ.setdefault("E2B_API_KEY", _DEFAULT_API_KEY)
    if not os.environ.get("SSL_CERT_FILE") and Path(_CA_CERT_PATH).is_file():
        os.environ["SSL_CERT_FILE"] = _CA_CERT_PATH
    _log(f"API URL: {os.environ.get('E2B_API_URL')}")

    base = os.environ["E2B_API_URL"].rstrip("/")
    agent_alias = f"agentflow-sandbox-{args.agent_id}"
    results: list[bool] = []
    ts = datetime.now().strftime("%Y%m%d_%H%M%S")
    log_file = _LOG_DIR / f"template_rest_check_{ts}.log"
    _LOG_DIR.mkdir(parents=True, exist_ok=True)

    print("=" * 68)
    print("CubeSandbox 模板 REST 接口可用性验证")
    print(f"  API: {base}")
    print(f"  目标模板: {args.target_template}  幂等 alias: {agent_alias}")
    print("=" * 68)

    # ── 1. GET /templates 列表 ─────────────────────────────────────
    print("\n[1] GET /templates 列表")
    code, data = _check("GET", f"{base}/templates")
    if code == 200 and isinstance(data, list):
        fields = sorted({k for t in data[:50] for k in t.keys()})
        ok, detail = _expect(
            "列表可访问且为 JSON 数组",
            True,
            f"HTTP {code}, {len(data)} 个模板, 字段={fields}",
        )
        results.append(ok)
        target_ids = [t.get("templateID", "") for t in data if isinstance(t, dict)]
        found = args.target_template in target_ids
        ok, detail = _expect(
            f"目标模板 {args.target_template} 存在于列表",
            found,
            "已找到" if found else "未找到（需确认 templateID）",
        )
        results.append(ok)
    else:
        ok, detail = _expect("列表可访问且为 JSON 数组", False, f"HTTP {code}, body={str(data)[:200]}")
        results.append(ok)
        results.append(False)

    # ── 2. GET /templates/{templateID} 单查（已存在）───────────────
    print("\n[2] GET /templates/{templateID} 单查（已存在模板）")
    code, data = _check("GET", f"{base}/templates/{args.target_template}")
    ok = code == 200 and isinstance(data, dict)
    detail = f"HTTP {code}" + (f", status={data.get('status')}" if isinstance(data, dict) else "")
    ok, detail = _expect(f"单查 {args.target_template} 返回 200 + JSON", ok, detail)
    results.append(ok)

    # ── 3. GET /templates/{agentflow-sandbox-{agent_id}}（幂等未命中分支）──
    print(f"\n[3] GET /templates/{agent_alias}（幂等检查：未预建 → 应 404）")
    code, data = _check("GET", f"{base}/templates/{agent_alias}")
    ok = code == 404
    ok, detail = _expect(
        "未预建 alias 返回 404（幂等'未命中'可据此判定）",
        ok,
        f"HTTP {code}" + (f", body={str(data)[:120]}" if code != 404 else ", 404 非 JSON 属正常"),
    )
    results.append(ok)

    # ── 4. POST /templates 字段探测（创建端点存在性）───────────────
    print("\n[4] POST /templates 字段探测（创建端点存在性，不实际创建）")
    code, data = _check("POST", f"{base}/templates", {})
    ok = code in (422, 500) and code != 404
    detail = f"HTTP {code}" + (f", body={str(data)[:120]}" if isinstance(data, str) else f", body={json.dumps(data, ensure_ascii=False)[:120]}")
    ok, detail = _expect("POST /templates 端点存在（422/500 字段校验而非 404）", ok, detail)
    results.append(ok)
    code, data = _check("POST", f"{base}/templates", {"image": "placeholder:test"})
    ok = code in (422, 500)
    detail = f"HTTP {code}" + (f", body={str(data)[:120]}" if isinstance(data, str) else f", body={json.dumps(data, ensure_ascii=False)[:120]}")
    ok, detail = _expect("POST 带 image 后进入下一字段校验（writable_layer_size）", ok, detail)
    results.append(ok)

    # ── 汇总 ────────────────────────────────────────────────────────
    passed = sum(1 for r in results if r)
    total = len(results)
    print("\n" + "=" * 68)
    print(f"结果: {passed}/{total} 通过")
    print("=" * 68)
    # 落盘（项目记忆：测试日志必须落在项目可见目录，不用 /tmp）
    with open(log_file, "w", encoding="utf-8") as f:
        f.write(f"CubeSandbox REST 验证 {ts}\n")
        f.write(f"API: {base}\n")
        f.write(f"结果: {passed}/{total} 通过\n")
    _log(f"日志落盘: {log_file}")
    return 0 if passed == total else 1


# ── 子命令：snapshot（原 template_snapshot_create.py）───────────────


def _upload_assets(sb, upload_files: list, upload_dirs: list) -> int:
    """上传资产到沙箱可写目录 $HOME/workspace。

    - upload_files: ["本地路径:远端路径", ...]（单个文件）
    - upload_dirs:  ["本地目录:远端目录", ...]（递归，保留相对路径）

    远端路径必须是用户可写目录（/home/user，非 /opt——非 root 无写权限）。
    目录递归上传复用 e2b_sync._should_exclude 过滤缓存/venv 产物
    （__pycache__/.venv*/.git 等，F5 单一真相来源），避免把宿主侧
    平台绑定的 .venv（如 41MB macOS venv）传到 Linux 沙箱。
    返回上传文件数。
    """
    try:
        from e2b_sync import _should_exclude
    except ImportError as e:
        _log(f"ERROR: 无法 import e2b_sync._should_exclude: {e}")
        return -1

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
        sb.files.write(remote, lp.read_bytes(), request_timeout=_UPLOAD_REQUEST_TIMEOUT)
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
            # 过滤缓存/venv 产物（__pycache__/.venv*/.git 等），瘦身上传
            if _should_exclude(rel):
                continue
            rp = f"{remote}/{rel}"
            sb.commands.run(f"mkdir -p {rp.rsplit('/', 1)[0]}", timeout=15)
            sb.files.write(rp, f.read_bytes(), request_timeout=_UPLOAD_REQUEST_TIMEOUT)
            count += 1
        _log(f"uploaded dir: {local} -> {remote} ({count} files total)")

    return count


def _upload_manifest(sb, manifest_path: str, local_root: str) -> int:
    """按 manifest.jsonc includes 全量上传资产到沙箱 $HOME/workspace。

    复用 e2b_sync.py 的 _load_manifest_whitelist（F5 单一真相来源）：
    解析 includes glob → 展开相对路径白名单（excludes 剔除）→ 逐文件上传。
    远端目标 = $HOME/workspace/<rel_posix>（用户可写目录，非 /opt——非 root）。
    返回上传文件数。被 sandbox_diag.py snapshot-verify 子命令复用（同名保持）。
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
        sb.files.write(rp, lp.read_bytes(), request_timeout=_UPLOAD_REQUEST_TIMEOUT)
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


def cmd_snapshot(args: argparse.Namespace) -> int:
    """增量快照模板创建：建沙箱 → patch envd → 上传资产 → setup.sh → 固化快照。

    --hash-manifest 模式：只计算 manifest includes 内容 hash8，stdout 最后一行 = hash8
    （供 C++ preload / e2e_perf_baseline.py 做 version/hash 检测，输出契约保持不变）。
    """
    # hash 模式：计算 manifest includes 内容 hash（供 C++ preload 做 version/hash 检测）
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
    patched = _patch_envd_url(sb)
    _log(f"[2/5] envd_api_url patched: {patched}")
    time.sleep(3)

    # 步骤 3：上传资产（--manifest 全量 + --upload-file/--upload-dir 追加并存）
    # 2026-08-11：manifest 与 upload-dir 不再二选一——agentflow-runtime 由宿主经
    # --upload-dir 随 agent package 一起上传（setup.sh 沙箱内从 $SCRIPT_DIR/packages/ 安装）
    try:
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
        else:
            _log("[3/5] 无 --manifest，跳过 manifest 资产上传")
        if args.upload_file or args.upload_dir:
            n2 = _upload_assets(sb, args.upload_file, args.upload_dir)
            _log(f"[3/5] upload-file/upload-dir 追加上传 {n2} files（含 agentflow-runtime）")
        else:
            _log("[3/5] 无 --upload-file/--upload-dir 追加上传")

        # 步骤 3.5：沙箱内执行 setup.sh（装依赖到系统 python，2026-08-11）
        # setup.sh 已随 manifest includes 上传到 $HOME/workspace/setup.sh（includes 含 setup.sh）；
        # agentflow-runtime 已由 --upload-dir 上传到 $HOME/workspace/packages/agentflow-runtime。
        # 执行时机：上传资产后、固化快照前——依赖在快照固化前就位，运行时
        # Sandbox.create(template=snap-*) 秒级可用，无需 v1 兜底上传。
        # 失败 → 快照构建失败（TemplateRegistry::preload 降级基础模板，execute 首问 v1 兜底，不阻塞）。
        try:
            setup_r = sb.commands.run("cd $HOME/workspace && bash setup.sh", timeout=600)
            setup_out = (setup_r.stdout or "").strip()
            setup_err = (setup_r.stderr or "").strip()
            if setup_r.exit_code == 0:
                _log(f"[3.5/5] setup.sh OK: {setup_out[-200:]}")
            else:
                _log(
                    f"ERROR: setup.sh failed exit={setup_r.exit_code} "
                    f"stderr={setup_err[-300:] if setup_err else '(empty)'}"
                )
                if not args.keep_sandbox:
                    sb.kill()
                return 1
        except Exception as e:
            _log(f"ERROR: setup.sh invocation error: {e}")
            if not args.keep_sandbox:
                sb.kill()
            return 1

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
    finally:
        # 异常路径兜底：确保临时沙箱不泄漏（keep-sandbox 显式保留除外；
        # kill 幂等，成功路径已 kill 时这里无副作用）
        if not args.keep_sandbox:
            try:
                sb.kill()
            except Exception:
                pass


# ── 子命令：warm（原 template_warm_create.py）───────────────────────


def _probe_http_ready(sb, health_url: str, attempts: int = 6, interval: float = 0.5) -> bool:
    for _ in range(attempts):
        try:
            r = sb.commands.run(f"curl -s --connect-timeout 2 {health_url}", timeout=3)
            if r.exit_code == 0:
                return True
        except Exception:
            pass
        time.sleep(interval)
    return False


def _create_sandbox(cfg: dict, server_path: Path, port: int, remote_dir: str):
    """段1：起沙箱 + 上传 server + 启动 + 就绪探测（复用 e2b_lifecycle.py 同款逻辑）。"""
    from e2b_code_interpreter import Sandbox

    create_kwargs = dict(timeout=600, envs={"SANDBOX_WORKDIR": remote_dir}, template=cfg["base_template"])
    if cfg.get("network_allow_out"):
        try:
            create_kwargs["network"] = {"allow_out": json.loads(cfg["network_allow_out"])}
        except json.JSONDecodeError:
            pass
    sb = Sandbox.create(**create_kwargs)
    _log(f"created sandbox_id={sb.sandbox_id}")

    # path-based 路由 patch（与 e2b_lifecycle.py cmd_create 一致）
    _patch_envd_url(sb)

    # 上传 + 启动 HTTP server（v1 兜底路径——预热模板必须显式启动）
    with open(server_path, "rb") as f:
        sb.files.write("/opt/sandbox/http_server/sandbox_http_server.py", f.read())
    _log("uploaded sandbox_http_server.py")
    start_cmd = (
        f"nohup python3 /opt/sandbox/http_server/sandbox_http_server.py"
        f" --port {port} --host 0.0.0.0 > /tmp/sandbox_http_server.log 2>&1 &"
    )
    sb.commands.run(start_cmd, background=True)
    _log(f"started HTTP server on port {port}")

    health_url = f"http://localhost:{port}/exec"
    if not _probe_http_ready(sb, health_url, attempts=20, interval=0.5):
        raise RuntimeError("HTTP server startup timeout")
    _log("HTTP server ready")
    return sb


def _commit_rest(cfg: dict, sandbox_id: str) -> tuple[int, str]:
    """段2a：REST commit 通道——自动探测 sandbox_id 字段名。

    实测（2026-08-09）：30030 REST 面未暴露独立 commit 端点（openapi.yml 无、405/404），
    该函数返回失败时调用方回退 CLI 提示。字段候选覆盖 snake/camel 两种命名。
    """
    base = cfg["api_url"].rstrip("/")
    candidates = [
        {"sandbox_id": sandbox_id},
        {"sandboxId": sandbox_id},
        {"sandboxID": sandbox_id},
    ]
    for body in candidates:
        code, text = _http("POST", f"{base}/sandboxes/commit", body, timeout=30)
        _log(f"REST commit {list(body.keys())[0]} -> HTTP {code}")
        if code in (200, 201, 202):
            return code, text
        if code not in (404, 405):  # 502/500 等后端错误不再换字段名（换名也无效）
            break
    return -1, "REST commit 端点不可用（30030 面未暴露独立 commit 端点）"


def _commit_cli(cfg: dict, sandbox_id: str, cli_path: str) -> int:
    """段2b：cubemastercli tpl commit 通道（CLI 是官方确认通道）。"""
    cli = cli_path or shutil.which("cubemastercli")
    if not cli:
        _log("ERROR: cubemastercli 不在 PATH（--cubemastercli 指定或安装后重试）")
        _log(f"提示：手动执行 → cubemastercli tpl commit --sandbox-id {sandbox_id}")
        return 1
    cmd = [cli, "tpl", "commit", "--sandbox-id", sandbox_id]
    _log(f"CLI commit: {' '.join(cmd)}")
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
    if proc.returncode != 0:
        _log(f"CLI commit failed: {proc.stderr[-500:]}")
        return proc.returncode
    _log(f"CLI commit output: {proc.stdout[-500:]}")
    return 0


def cmd_warm(args: argparse.Namespace) -> int:
    """预热模板创建：起沙箱 + 上传 HTTP server + 启动就绪 → commit 固化模板。"""
    cfg = _load_config()
    if args.base_template:
        cfg["base_template"] = args.base_template
    _setup_env(cfg)
    _apply_ssl_patch()
    _LOG_DIR.mkdir(parents=True, exist_ok=True)

    server_path = Path(args.server_path)
    if not server_path.is_file():
        _log(f"ERROR: server 不存在: {server_path}")
        return 1

    _log(f"base_template={cfg['base_template']} server={server_path} port={args.port}")
    try:
        # 段1：起沙箱
        sb = _create_sandbox(cfg, server_path, args.port, args.remote_dir)
        sandbox_id = sb.sandbox_id
        _log(f"[1/2] sandbox ready: {sandbox_id}")

        # 段2：固化（commit）
        method = args.commit_method
        rest_ok = False
        if method in ("rest", "auto"):
            code, text = _commit_rest(cfg, sandbox_id)
            if code in (200, 201, 202):
                rest_ok = True
                _log(f"[2/2] REST commit accepted: {text[:200]}")
            elif method == "auto":
                _log(f"[2/2] REST commit 不可用（{text[:120]}），回退 CLI")
        if not rest_ok:
            if method == "cli" or method == "auto":
                rc = _commit_cli(cfg, sandbox_id, args.cubemastercli)
                if rc != 0:
                    return 1
            else:
                _log("ERROR: commit 失败（--commit-method=rest 且 REST 不可用）")
                return 1

        _log("预热模板创建流程完成。请确认新模板 ID 后更新 daemon 配置 e2b_default_template。")
        return 0
    except Exception as e:
        _log(f"FATAL: {e}")
        import traceback
        traceback.print_exc(file=sys.stderr)
        return 1


# ── CLI 入口 ────────────────────────────────────────────────────────


def main() -> int:
    parser = argparse.ArgumentParser(
        description="CubeSandbox 模板运维工具合集（rest-check / snapshot / warm）",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    sub = parser.add_subparsers(dest="command", help="子命令")

    # rest-check（原 template_rest_check.py）
    p = sub.add_parser("rest-check", help="模板 REST 接口可用性验证（纯 HTTP 不建沙箱）")
    p.add_argument("--api-url", default=os.environ.get("E2B_API_URL", ""),
                   help="API 地址（默认环境变量 E2B_API_URL 或内置默认）")
    p.add_argument("--api-key", default=os.environ.get("E2B_API_KEY", ""),
                   help="API 密钥（默认环境变量 E2B_API_KEY）")
    p.add_argument("--agent-id", default="outfit-assistant",
                   help="agent_id，用于构造幂等 alias（agentflow-sandbox-{agent_id}）")
    p.add_argument("--target-template", default="tpl-cb6b9dcbf5784a1487a75971",
                   help="目标已有模板 templateID")
    p.set_defaults(func=cmd_rest_check)

    # snapshot（原 template_snapshot_create.py）
    p = sub.add_parser("snapshot", help="增量快照模板创建（REST snapshot 通道）")
    p.add_argument("--base-template", default="", help="基础模板（默认取 config e2b_default_template）")
    p.add_argument("--name", default="agentflow-warm-incr", help="快照名（建议带版本号）")
    p.add_argument("--description", default="incremental warm template", help="快照描述")
    p.add_argument("--upload-file", action="append", default=[],
                   help="上传单个文件：本地路径:远端路径（可多次）")
    p.add_argument("--upload-dir", action="append", default=[],
                   help="上传目录（递归）：本地目录:远端目录（可多次）")
    p.add_argument("--manifest", default="",
                   help="manifest.jsonc 路径（agent 包根，includes 全量上传到 $HOME/workspace）")
    p.add_argument("--local-root", default="",
                   help="manifest includes glob 的本地根目录（默认取 manifest 所在目录）")
    p.add_argument("--keep-sandbox", action="store_true",
                   help="保留临时沙箱不清理（调试用）")
    # hash 模式参数：计算 manifest includes 内容 hash（供 C++ preload 做 version/hash 检测）
    p.add_argument("--hash-manifest", default="",
                   help="hash 模式：计算 manifest includes 内容 hash8（供 C++ preload 检测）")
    p.add_argument("--hash-local-root", default="",
                   help="hash 模式：local root（默认取 --hash-manifest 所在目录）")
    p.set_defaults(func=cmd_snapshot)

    # warm（原 template_warm_create.py）
    p = sub.add_parser("warm", help="预热模板创建（REST + CLI commit 双通道）")
    p.add_argument("--base-template", default="", help="基础模板（默认取 config e2b_default_template）")
    p.add_argument("--server-path", default=str(_DEFAULT_SERVER), help="sandbox_http_server.py 路径")
    p.add_argument("--port", type=int, default=8080, help="沙箱内 HTTP server 端口")
    p.add_argument("--remote-dir", default="/home/user/workspace", help="远端 workspace 目录")
    p.add_argument("--commit-method", choices=["rest", "cli", "auto"], default="auto",
                   help="commit 通道：rest（REST 探测）/ cli（cubemastercli）/ auto（rest 失败回退 cli）")
    p.add_argument("--cubemastercli", default="", help="cubemastercli 可执行文件路径")
    p.set_defaults(func=cmd_warm)

    args = parser.parse_args()
    if not getattr(args, "command", None):
        parser.print_help()
        return 1
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
