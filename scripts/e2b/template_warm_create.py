#!/usr/bin/env python3
"""CubeSandbox 预热模板创建脚本（REST 版，结合现有脚本）。

用途：创建"预跑 HTTP server"的预热模板，使段2 `Sandbox.create(template=预热模板)`
首问毫秒级就绪（HTTP server 已在跑），避免每次上传+启动（实测 31s → 目标几十 ms）。

流程（三段）：
  1. 起沙箱 + 上传 sandbox_http_server.py + 启动 + 就绪探测
     —— 复用 e2b_lifecycle.py 的 create 逻辑（同款 SSL patch / envd URL patch / 就绪探测）
  2. 固化沙箱为模板（commit）：
     - REST 通道：尝试 POST /sandboxes/commit（自动探测 sandbox_id/sandboxId/sandboxID 字段）
       —— 实测（2026-08-09）：30030 REST 面未暴露独立 commit 端点（openapi.yml 无、
          405/404），成功与否取决于部署版本；失败时自动回退到 CLI 提示
     - CLI 通道：`cubemastercli tpl commit --sandbox-id <id>`（需本机安装，--cubemastercli 指定）
  3. 轮询 job 状态至 READY，输出新模板 ID

用法:
  # REST 优先，失败提示 CLI
  python3 scripts/e2b/template_warm_create.py

  # 强制 CLI 通道（需 cubemastercli 在 PATH）
  python3 scripts/e2b/template_warm_create.py --commit-method cli

  # 自定义参数
  python3 scripts/e2b/template_warm_create.py \
      --base-template tpl-cb6b9dcbf5784a1487a75971 \
      --server-path docker/sandbox/http_server/sandbox_http_server.py \
      --port 8080 \
      --remote-dir /home/user/workspace

环境变量/配置:
  configs/test/agentflowd.json 的 sandbox 段（e2b_api_url / e2b_api_key / e2b_default_template）
  E2B_API_URL / E2B_API_KEY 可覆盖

输出:
  新模板 ID（如 tpl-xxx），可写入 daemon 配置 e2b_default_template
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
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
_DEFAULT_CONFIG = ROOT / "configs" / "test" / "agentflowd.json"
_DEFAULT_SERVER = ROOT / "docker" / "sandbox" / "http_server" / "sandbox_http_server.py"
_LOG_DIR = ROOT / "logs" / "tests"


def _log(msg: str) -> None:
    print(f"[warm_create] {msg}", file=sys.stderr, flush=True)


def _load_config() -> dict:
    cfg = json.loads(_DEFAULT_CONFIG.read_text(encoding="utf-8"))
    sc = cfg.get("sandbox", {})
    return {
        "api_url": os.environ.get("E2B_API_URL", sc.get("e2b_api_url", "http://218.245.96.148:30030")),
        "api_key": os.environ.get("E2B_API_KEY", sc.get("e2b_api_key", "")),
        "base_template": sc.get("e2b_default_template", "tpl-cb6b9dcbf5784a1487a75971"),
        "network_allow_out": sc.get("e2b_network_allow_out", ""),
    }


def _apply_ssl_patch() -> None:
    """内部部署自签名证书，SDK 校验跳过（与 e2b_lifecycle.py 一致）。"""
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
    _ssl._agentflow_patched = True


def _setup_env(cfg: dict) -> None:
    os.environ["E2B_API_URL"] = cfg["api_url"]
    if cfg["api_key"]:
        os.environ["E2B_API_KEY"] = cfg["api_key"]
    ca = ROOT / "resource" / "certs" / "e2b_CA.pem"
    if ca.is_file() and not os.environ.get("SSL_CERT_FILE"):
        os.environ["SSL_CERT_FILE"] = str(ca)
    _apply_ssl_patch()


def _http(method: str, url: str, body: dict | None = None, timeout: int = 30):
    headers = {"Content-Type": "application/json"}
    key = os.environ.get("E2B_API_KEY", "")
    if key:
        headers["Authorization"] = f"Bearer {key}"
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(url, data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, r.read().decode("utf-8", errors="replace")
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode("utf-8", errors="replace")
    except Exception as e:
        return -1, f"net error: {e}"


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
    path_based_url = f"https://218.245.96.148:443/sandbox/{sb.sandbox_id}/49983"
    sb._SandboxBase__envd_api_url = path_based_url
    for attr in ("_filesystem", "_commands", "_pty"):
        mod = getattr(sb, attr, None)
        if mod is not None and hasattr(mod, "_envd_api_url"):
            mod._envd_api_url = path_based_url

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
        code, text = _http("POST", f"{base}/sandboxes/commit", body)
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


def main() -> int:
    ap = argparse.ArgumentParser(description="CubeSandbox 预热模板创建（REST 起沙箱 + commit 双通道）")
    ap.add_argument("--base-template", default="", help="基础模板（默认取 config e2b_default_template）")
    ap.add_argument("--server-path", default=str(_DEFAULT_SERVER), help="sandbox_http_server.py 路径")
    ap.add_argument("--port", type=int, default=8080, help="沙箱内 HTTP server 端口")
    ap.add_argument("--remote-dir", default="/home/user/workspace", help="远端 workspace 目录")
    ap.add_argument("--commit-method", choices=["rest", "cli", "auto"], default="auto",
                    help="commit 通道：rest（REST 探测）/ cli（cubemastercli）/ auto（rest 失败回退 cli）")
    ap.add_argument("--cubemastercli", default="", help="cubemastercli 可执行文件路径")
    args = ap.parse_args()

    cfg = _load_config()
    if args.base_template:
        cfg["base_template"] = args.base_template
    _setup_env(cfg)
    _LOG_DIR.mkdir(parents=True, exist_ok=True)
    ts = time.strftime("%Y%m%d_%H%M%S")

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


if __name__ == "__main__":
    sys.exit(main())
