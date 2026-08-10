#!/usr/bin/env python3
"""验证快照模板中 weather.py 的路径与内容修复（2026-08-10）。

背景：template_snapshot_create.py 曾把资产上传到 $HOME/workdir（与运行时
SANDBOX_WORKSPACE=/home/user/workspace 错位），且用 base64.b64encode 写入
（文件内容损坏）。修复后本脚本验证三点：
  1. PATH-CHECK   weather.py 存在于 /home/user/workspace/...（路径统一）
  2. CONTENT-CHECK 文件头是 Python 源码而非 base64 文本（内容不再损坏）
  3. EXEC-CHECK   python3 weather.py 可执行（open-meteo 可达，输出 JSON）

用法:
  python3 scripts/e2b/verify_snapshot_weather.py [--template snap-xxx]

凭据：从 configs/test/agentflowd.json 加载（与 template_snapshot_create.py 一致），
不经过命令行参数。
"""

import argparse
import json
import os
import ssl
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
_CONFIG = ROOT / "configs" / "test" / "agentflowd.json"


def _log(msg: str) -> None:
    print(f"[verify] {msg}", file=sys.stderr, flush=True)


def _load_config() -> dict:
    cfg = json.loads(_CONFIG.read_text(encoding="utf-8"))
    sc = cfg.get("sandbox", {})
    return {
        "api_url": os.environ.get("E2B_API_URL", sc.get("e2b_api_url", "http://218.245.96.148:30030")),
        "api_key": os.environ.get("E2B_API_KEY", sc.get("e2b_api_key", "")),
        "default_template": sc.get("e2b_default_template", ""),
        "network_allow_out": sc.get("e2b_network_allow_out", ""),
    }


def _setup_env(cfg: dict) -> None:
    os.environ["E2B_API_URL"] = cfg["api_url"]
    if cfg["api_key"]:
        os.environ["E2B_API_KEY"] = cfg["api_key"]
    ca = ROOT / "resource" / "certs" / "e2b_CA.pem"
    if ca.is_file() and not os.environ.get("SSL_CERT_FILE"):
        os.environ["SSL_CERT_FILE"] = str(ca)
    # 自建部署自签名证书，跳过校验（与 e2b_lifecycle.py 一致）
    _orig = ssl.create_default_context

    def _patched(*a, **kw):
        ctx = _orig(*a, **kw)
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
        return ctx

    ssl.create_default_context = _patched


def main() -> int:
    ap = argparse.ArgumentParser(description="验证快照模板 weather.py 路径与内容修复")
    ap.add_argument("--template", default="", help="快照模板 ID（默认取 config e2b_default_template）")
    args = ap.parse_args()

    cfg = _load_config()
    _setup_env(cfg)
    template = args.template or cfg["default_template"]
    if not template:
        _log("ERROR: 未指定 --template 且 config e2b_default_template 为空")
        return 1

    from e2b_code_interpreter import Sandbox

    _log(f"template={template}")
    create_kwargs = dict(timeout=120, template=template)
    if cfg.get("network_allow_out"):
        try:
            allowed = json.loads(cfg["network_allow_out"])
            if isinstance(allowed, list) and allowed:
                create_kwargs["network"] = {"allow_out": allowed}
                _log(f"network.allow_out applied ({len(allowed)} nets)")
        except json.JSONDecodeError:
            _log("WARN: e2b_network_allow_out parse failed, sandbox uses platform default")
    sb = Sandbox.create(**create_kwargs)
    _log(f"sandbox_id={sb.sandbox_id}")

    # patch envd url（与 template_snapshot_create.py _patch_envd_url_full 一致——
    # 用裸 IP 直连 :443，实测 cube.app 域名 TLS 握手被重置 errno=54）
    target_host = "https://218.245.96.148:443"
    path_based_url = f"{target_host}/sandbox/{sb.sandbox_id}/49983"
    sb._SandboxBase__envd_api_url = path_based_url
    for attr in ("_filesystem", "_commands", "_pty"):
        mod = getattr(sb, attr, None)
        if mod is not None and hasattr(mod, "_envd_api_url"):
            mod._envd_api_url = path_based_url

    wp = "/home/user/workspace/harness/skills/outfit-stylist/weather.py"
    try:
        # 1) 路径验证
        r1 = sb.commands.run(f"ls -la {wp} 2>&1; echo EXIT=$?", timeout=20)
        print("PATH-CHECK:", r1.stdout.strip())
        # 2) 内容验证：文件头应为 Python 源码而非 base64
        r2 = sb.commands.run(f"head -3 {wp}", timeout=20)
        print("CONTENT-CHECK:", r2.stdout.strip()[:200])
        # 3) 执行验证：weather.py 可运行
        r3 = sb.commands.run(
            f'python3 {wp} "北京" 2>&1 | head -c 500; echo; echo EXIT=$?', timeout=30
        )
        print("EXEC-CHECK:", r3.stdout.strip()[:400])
    finally:
        sb.kill()
        _log("sandbox killed")

    return 0


if __name__ == "__main__":
    sys.exit(main())
