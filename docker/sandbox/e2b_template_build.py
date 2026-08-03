#!/usr/bin/env python3
"""e2b 自定义模板构建工具 — 一次性设置脚本。

用途：将 sandbox_http_server.py 预装到 e2b 模板中，后续沙箱创建时
跳过上传步骤，节省一次 RTT。

用法：
  # 基于 base 模板构建
  python3 docker/sandbox/e2b_template_build.py --name agentflow-sandbox

  # 基于已有模板 + 指定资源配额
  python3 docker/sandbox/e2b_template_build.py \\
      --name agentflow-sandbox-v2 \\
      --from-template my-custom-base \\
      --cpu 4 --memory 4096

输出：
  构建成功后打印模板 ID，可用于 daemon 配置的 e2b_default_template。

环境变量：
  E2B_API_KEY  必填，e2b SDK 鉴权
"""

import argparse
import os
import sys
from pathlib import Path


def _log(msg: str) -> None:
    print(f"[e2b_template_build] {msg}", file=sys.stderr)


def _check_api_key() -> str:
    api_key = os.environ.get("E2B_API_KEY")
    if not api_key:
        print("[e2b_template_build] ERROR: E2B_API_KEY environment variable not set", file=sys.stderr)
        sys.exit(2)
    return api_key


def _read_http_server() -> str:
    """读取本地 sandbox_http_server.py 内容，用于预置到模板中。"""
    script_path = Path(__file__).resolve().parent / "http_server" / "sandbox_http_server.py"
    if not script_path.is_file():
        _log(f"ERROR: sandbox_http_server.py not found at {script_path}")
        sys.exit(1)
    return script_path.read_text()


def _read_setup_script() -> str:
    """读取本地 sandbox-setup.sh 内容，用于预置到模板中。"""
    script_path = Path(__file__).resolve().parent / "sandbox-setup.sh"
    if not script_path.is_file():
        _log(f"ERROR: sandbox-setup.sh not found at {script_path}")
        sys.exit(1)
    return script_path.read_text()


def build_template(args: argparse.Namespace) -> None:
    _check_api_key()

    try:
        from e2b import Template
    except ImportError:
        _log("ERROR: e2b SDK not installed. Run: pip install e2b")
        sys.exit(1)

    server_code = _read_http_server()
    setup_script = _read_setup_script()
    template_name = args.name

    _log(f"Building template: {template_name}")
    _log(f"  from_template: {args.from_template}")
    _log(f"  cpu: {args.cpu}, memory: {args.memory}MB")
    _log(f"  sandbox_http_server.py: {len(server_code)} bytes")
    _log(f"  sandbox-setup.sh: {len(setup_script)} bytes")

    # 预装 sandbox_http_server.py 到 /opt/sandbox/http_server/
    server_path = "/opt/sandbox/http_server/sandbox_http_server.py"
    setup_path = "/opt/sandbox/sandbox-setup.sh"

    template = (
        Template()
        .from_template(args.from_template)
        .add_files({
            server_path: server_code,
            setup_path: setup_script,
        })
        .add_commands([
            "mkdir -p /opt/sandbox/tools",
            "bash /opt/sandbox/sandbox-setup.sh",
        ])
        # 设置环境变量，让 HTTP server 能找到工作目录
        .env_vars({"SANDBOX_WORKDIR": "/workdir"})
    )

    # 构建模板
    result = Template.build(
        template,
        template_name,
        cpu_count=args.cpu,
        memory_mb=args.memory,
    )

    # 输出模板 ID
    print(f"template_id={result.id}")
    _log(f"Template built successfully: {result.id}")
    _log(f"Use this template ID in daemon config as e2b_default_template")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="e2b 自定义模板构建工具"
    )
    parser.add_argument("--name", default="agentflow-sandbox",
                        help="模板名称（默认 agentflow-sandbox）")
    parser.add_argument("--from-template", default="base",
                        help="基础模板 ID（默认 base）")
    parser.add_argument("--cpu", type=int, default=2,
                        help="CPU 核数（默认 2）")
    parser.add_argument("--memory", type=int, default=2048,
                        help="内存 MB（默认 2048）")
    parser.set_defaults(func=build_template)

    args = parser.parse_args()
    try:
        args.func(args)
    except SystemExit:
        raise
    except Exception as e:
        _log(f"FATAL: {e}")
        import traceback
        traceback.print_exc(file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())