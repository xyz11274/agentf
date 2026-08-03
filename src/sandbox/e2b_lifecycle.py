#!/usr/bin/env python3
# Copyright (c) 2026 AgentFlow Authors
# Licensed under MIT License
#
# e2b 沙箱生命周期管理脚本
#
# 用途：把原本内嵌在 src/sandbox/sandbox_e2b.cpp 里的 Python 代码抽出来，
#       C++ 侧只负责 fork+execvp python3 e2b_lifecycle.py <subcommand> <args>。
#
# 子命令：
#   create  创建 e2b 沙箱、上传 sandbox_http_server.py、后台启动、轮询就绪
#   kill    幂等销毁 e2b 沙箱（已 kill 时无操作）
#
# 调用契约（被 E2BSandbox 通过 fork+execvp 调用）：
#   python3 e2b_lifecycle.py create \
#       --traffic-access-token TOKEN \
#       --sandbox-http-server-path PATH \
#       --sandbox-http-port PORT \
#       --remote-workspace-dir DIR \
#       [--timeout SECONDS]
#   python3 e2b_lifecycle.py kill --sandbox-id ID
#
# 环境变量：
#   E2B_API_KEY     必填，e2b SDK 鉴权
#
# 退出码：
#   0  成功（create 时 stdout 输出 JSON {"id":..., "host_url":...}）
#   1  参数错误 / API 调用失败 / 就绪超时
#   2  E2B_API_KEY 缺失
#
# 日志：打到 stderr，父进程读取或丢弃；不污染 stdout（stdout 留给结构化结果）

import argparse
import json
import os
import sys
import time
import traceback
from pathlib import Path

try:
    from e2b_code_interpreter import Sandbox
except ImportError:
    print("[e2b_lifecycle] ERROR: e2b_code_interpreter not installed", file=sys.stderr)
    sys.exit(1)

# 共用 sandbox_patch（Phase 4：从 agentflow-runtime 导入，fallback 到本地定义）
try:
    from agentflow_runtime.common.sandbox_patch import apply_ssl_patch as _apply_ssl_patch
except ImportError:
    # daemon 环境可能未安装 agentflow-runtime，fallback 到本地定义
    def _apply_ssl_patch() -> None:
        """SSLPatch: path-based 路由走 IP 直连，跳过证书验证（内部部署）。

        [fallback] agentflow-runtime 不可用时使用此本地定义。
        权威实现见 ``agentflow_runtime.common.sandbox_patch.apply_ssl_patch``。
        """
        import ssl as _ssl

        if getattr(_ssl, "_agentflow_patched", False):
            return  # idempotent guard

        _orig_create_default_context = _ssl.create_default_context

        def _patched_create_default_context(*args, **kwargs):
            ctx = _orig_create_default_context(*args, **kwargs)
            ctx.check_hostname = False
            ctx.verify_mode = _ssl.CERT_NONE
            return ctx

        _ssl.create_default_context = _patched_create_default_context
        _ssl._agentflow_patched = True  # type: ignore[attr-defined]


def _log(msg: str) -> None:
    print(f"[e2b_lifecycle] {msg}", file=sys.stderr, flush=True)


def _resolve_ssl_cert() -> None:
    """将 SSL_CERT_FILE 相对路径解析为绝对路径。

    子进程可能不继承工作目录，导致相对路径的 SSL_CERT_FILE 无法被 SDK 使用。
    """
    cert_path = os.environ.get("SSL_CERT_FILE", "")
    if cert_path and not os.path.isabs(cert_path):
        # 尝试从脚本所在目录向上搜索 project root
        script_dir = Path(__file__).resolve().parent
        abs_path = (script_dir / cert_path).resolve()
        if abs_path.is_file():
            os.environ["SSL_CERT_FILE"] = str(abs_path)
            _log(f"resolved SSL_CERT_FILE: {cert_path} → {abs_path}")
        # 也尝试从 CWD 解析
        cwd_path = Path.cwd() / cert_path
        if cwd_path.is_file():
            os.environ["SSL_CERT_FILE"] = str(cwd_path.resolve())
            _log(f"resolved SSL_CERT_FILE: {cert_path} → {cwd_path.resolve()}")


def _check_api_key() -> str:
    api_key = os.environ.get("E2B_API_KEY")
    if not api_key:
        print("[e2b_lifecycle] ERROR: E2B_API_KEY environment variable not set", file=sys.stderr)
        sys.exit(2)
    return api_key


def cmd_create(args: argparse.Namespace) -> int:
    api_key = _check_api_key()
    _log(f"create: E2B_API_KEY={api_key[:8]}...{api_key[-4:] if len(api_key) > 12 else ''} "
         f"(len={len(api_key)})")
    _log(f"create: E2B_API_URL={os.environ.get('E2B_API_URL', '(not set)')}")
    # path-based 路由走 IP 直连，但证书是给 *.cube.app 签的，需跳过主机名校验
    # 且内部部署的 CA 证书路径可能不一致，一并跳过证书验证
    _apply_ssl_patch()

    if not Path(args.sandbox_http_server_path).is_file():
        _log(f"ERROR: sandbox_http_server.py not found: {args.sandbox_http_server_path}")
        return 1

    _log(f"create: token={args.traffic_access_token[:8]}... "
         f"port={args.sandbox_http_port} remote_dir={args.remote_workspace_dir} "
         f"template={args.template}")

    # 构造 Sandbox.create 参数
    # envd 实际监听 49983（SDK 默认值），经 SSH 上宿主机实测确认
    # （之前部署方说的 49999 是另一个端口，不是 envd gRPC）

    create_kwargs = dict(
        timeout=args.timeout,
        envs={"SANDBOX_WORKDIR": args.remote_workspace_dir},
        template=args.template,
    )
    # 解析网络出站规则（JSON 数组字符串 → {"allow_out": [...]}）
    if args.network_allow_out:
        try:
            allowed = json.loads(args.network_allow_out)
            if not isinstance(allowed, list):
                _log(f"ERROR: --network-allow-out must be a JSON array, got: {type(allowed).__name__}")
                return 1
            create_kwargs["network"] = {"allow_out": allowed}
        except json.JSONDecodeError as e:
            _log(f"ERROR: invalid --network-allow-out JSON: {e}")
            return 1
    sb = Sandbox.create(**create_kwargs)
    _log(f"create: sandbox_id={sb.sandbox_id}")

    # 自定义 e2b 部署的 envd API 走 path-based 路由（非 hostname 路由）
    # nginx 配置: server_name _; location ^~ /sandbox/<id>/<port>/
    # 因此需要覆盖 SDK 自动构造的 https://{port}-{id}.cube.app 地址
    target_host = "https://218.245.96.148:443"
    path_based_url = f"{target_host}/sandbox/{sb.sandbox_id}/49983"
    # patch 沙箱内部存 envd_api_url 的地方（SDK 各处分别缓存，逐一覆盖）
    sb._SandboxBase__envd_api_url = path_based_url
    # filesystem / commands / pty 各自持有自己的 _envd_api_url
    if hasattr(sb, '_filesystem'):
        sb._filesystem._envd_api_url = path_based_url
    if hasattr(sb, '_commands'):
        sb._commands._envd_api_url = path_based_url
    if hasattr(sb, '_pty'):
        sb._pty._envd_api_url = path_based_url
    _log(f"create: envd_api_url={path_based_url} (path-based routing)")

    with open(args.sandbox_http_server_path, "rb") as f:
        sb.files.write("/opt/sandbox/http_server/sandbox_http_server.py", f.read())
    _log("create: uploaded sandbox_http_server.py")

    start_cmd = (
        f"nohup python3 /opt/sandbox/http_server/sandbox_http_server.py"
        f" --port {args.sandbox_http_port} --host 0.0.0.0"
        f" > /tmp/sandbox_http_server.log 2>&1 &"
    )
    sb.commands.run(start_cmd, background=True)
    _log("create: started HTTP server in background")

    health_url = f"http://localhost:{args.sandbox_http_port}/exec"
    for _ in range(20):
        try:
            r = sb.commands.run(
                f"curl -s --connect-timeout 2 {health_url}", timeout=3
            )
            if r.exit_code == 0:
                _log("create: HTTP server ready")
                break
        except Exception:
            pass
        time.sleep(0.5)
    else:
        _log("ERROR: HTTP server startup timeout after 10s")
        return 1

    host_url = f"http://218.245.96.148:80/sandbox/{sb.sandbox_id}/{args.sandbox_http_port}"
    json.dump({"id": sb.sandbox_id, "host_url": host_url}, sys.stdout)
    sys.stdout.write("\n")
    return 0


def cmd_kill(args: argparse.Namespace) -> int:
    api_key = _check_api_key()
    _log(f"kill: sandbox_id={args.sandbox_id}")

    try:
        sb = Sandbox._cls_connect_sandbox(sandbox_id=args.sandbox_id)
        sb.kill()
        _log(f"kill: done sandbox_id={args.sandbox_id}")
        return 0
    except Exception as e:
        msg = str(e).lower()
        if "404" in msg or "not found" in msg or "already" in msg:
            _log("kill: sandbox already gone, treat as success")
            return 0
        _log(f"kill: FATAL {e}")
        traceback.print_exc(file=sys.stderr)
        return 1


def cmd_snapshot(args: argparse.Namespace) -> int:
    """为已创建的 e2b 沙箱创建文件系统快照（FR-012）。

    通过 e2b SDK 的 snapshot API 在平台侧保存沙箱磁盘状态，
    快照可用于后续 Sandbox.create() 恢复。不下载文件到本地。
    """
    api_key = _check_api_key()
    _log(f"snapshot: sandbox_id={args.sandbox_id}")
    # SSLPatch 必须在 SDK 初始化前应用，与 cmd_create 一致
    _apply_ssl_patch()
    try:
        sb = Sandbox._cls_connect_sandbox(sandbox_id=args.sandbox_id)
        sb.create_snapshot()
        _log(f"snapshot: done sandbox_id={args.sandbox_id}")
        return 0
    except Exception as e:
        msg = str(e).lower()
        if "404" in msg or "not found" in msg:
            _log("snapshot: sandbox already gone, skip snapshot")
            return 0
        _log(f"snapshot: FATAL {e}")
        traceback.print_exc(file=sys.stderr)
        return 1


def cmd_fork(args: argparse.Namespace) -> int:
    """从源沙箱 fork 出新沙箱（e2b 平台侧 restore 通道，§8.1）。

    调用 e2b SDK Sandbox.fork() 从源沙箱的当前文件系统状态派生新沙箱。
    新沙箱继承源沙箱的 filesystem（含已上传的 workspace 文件），
    然后启动 HTTP server 并应用 path-based routing patch。

    输出 JSON（与 cmd_create 同格式）：{"id": ..., "host_url": ...}
    """
    api_key = _check_api_key()
    _apply_ssl_patch()
    _log(f"fork: from_sandbox_id={args.sandbox_id}")

    # 1. 连源沙箱
    try:
        sb = Sandbox._cls_connect_sandbox(sandbox_id=args.sandbox_id)
    except Exception as e:
        _log(f"ERROR: failed to connect to source sandbox {args.sandbox_id}: {e}")
        traceback.print_exc(file=sys.stderr)
        return 1

    # 2. fork 派生新沙箱（返回列表，默认 count=1 时取首个）
    try:
        forked_list = sb.fork(timeout=args.timeout)
        if not forked_list:
            _log("ERROR: fork returned empty list")
            return 1
        forked = forked_list[0]
        if isinstance(forked, Exception):
            _log(f"ERROR: fork failed: {forked}")
            return 1
    except Exception as e:
        _log(f"ERROR: fork exception: {e}")
        traceback.print_exc(file=sys.stderr)
        return 1

    new_id = forked.sandbox_id
    _log(f"fork: new_sandbox_id={new_id}")

    # 3. 应用 path-based routing patch（与 cmd_create 一致）
    target_host = "https://218.245.96.148:443"
    path_based_url = f"{target_host}/sandbox/{new_id}/49983"
    forked._SandboxBase__envd_api_url = path_based_url
    if hasattr(forked, '_filesystem'):
        forked._filesystem._envd_api_url = path_based_url
    if hasattr(forked, '_commands'):
        forked._commands._envd_api_url = path_based_url
    if hasattr(forked, '_pty'):
        forked._pty._envd_api_url = path_based_url
    _log(f"fork: envd_api_url={path_based_url} (path-based routing)")

    # 4. 启动 HTTP server（新沙箱是新进程，需要启动 server）
    # 从命令行参数读取 sandbox_http_server_path，若为空则从原路径上传
    if args.sandbox_http_server_path:
        with open(args.sandbox_http_server_path, "rb") as f:
            forked.files.write("/opt/sandbox/http_server/sandbox_http_server.py", f.read())
        _log("fork: uploaded sandbox_http_server.py")
    start_cmd = (
        f"nohup python3 /opt/sandbox/http_server/sandbox_http_server.py"
        f" --port {args.sandbox_http_port} --host 0.0.0.0"
        f" > /tmp/sandbox_http_server.log 2>&1 &"
    )
    forked.commands.run(start_cmd, background=True)
    _log("fork: started HTTP server in background")

    # 5. 轮询就绪
    health_url = f"http://localhost:{args.sandbox_http_port}/exec"
    for _ in range(20):
        try:
            r = forked.commands.run(
                f"curl -s --connect-timeout 2 {health_url}", timeout=3
            )
            if r.exit_code == 0:
                _log("fork: HTTP server ready")
                break
        except Exception:
            pass
        time.sleep(0.5)
    else:
        _log("ERROR: fork HTTP server startup timeout after 10s")
        return 1

    # 6. 输出 JSON（与 cmd_create 一致，供 C++ 侧统一解析）
    host_url = f"http://218.245.96.148:80/sandbox/{new_id}/{args.sandbox_http_port}"
    json.dump({"id": new_id, "host_url": host_url}, sys.stdout)
    sys.stdout.write("\n")
    return 0

def main() -> int:
    parser = argparse.ArgumentParser(
        description="e2b 沙箱生命周期管理（create / kill / snapshot / fork）"
    )
    sub = parser.add_subparsers(dest="subcommand", required=True)

    p_create = sub.add_parser("create", help="创建 e2b 沙箱并启动 HTTP server")
    p_create.add_argument("--traffic-access-token", required=True,
                          help="e2b 公共 URL token 鉴权")
    p_create.add_argument("--sandbox-http-server-path", required=True,
                          help="宿主机侧 sandbox_http_server.py 绝对路径")
    p_create.add_argument("--sandbox-http-port", type=int, required=True,
                          help="沙箱内 HTTP server 监听端口")
    p_create.add_argument("--remote-workspace-dir", required=True,
                          help="e2b 远端工作目录绝对路径")
    p_create.add_argument("--timeout", type=int, default=3600,
                          help="e2b 沙箱超时秒数（默认 3600）")
    p_create.add_argument("--template", default="base",
                          help="e2b 沙箱模板 ID（默认 base）")
    p_create.add_argument("--network-allow-out",
                          help="沙箱出站网络 CIDR 列表 JSON 数组，如 '[\"10.0.0.0/8\"]'")
    p_create.set_defaults(func=cmd_create)

    p_kill = sub.add_parser("kill", help="幂等销毁 e2b 沙箱")
    p_kill.add_argument("--sandbox-id", required=True, help="e2b sandbox ID")
    p_kill.set_defaults(func=cmd_kill)

    p_snapshot = sub.add_parser("snapshot", help="对 e2b 沙箱创建文件系统快照（FR-012）")
    p_snapshot.add_argument("--sandbox-id", required=True, help="e2b sandbox ID")
    p_snapshot.set_defaults(func=cmd_snapshot)

    p_fork = sub.add_parser("fork", help="从源沙箱 fork 出新沙箱（B3 restore 通道）")
    p_fork.add_argument("--sandbox-id", required=True, help="源 e2b sandbox ID")
    p_fork.add_argument("--sandbox-http-server-path", required=True,
                        help="宿主机侧 sandbox_http_server.py 绝对路径")
    p_fork.add_argument("--sandbox-http-port", type=int, required=True,
                        help="沙箱内 HTTP server 监听端口")
    p_fork.add_argument("--timeout", type=int, default=3600,
                        help="fork 超时秒数（默认 3600）")
    p_fork.set_defaults(func=cmd_fork)

    args = parser.parse_args()

    # 解析 SSL_CERT_FILE 相对路径为绝对路径（子进程 CWD 可能不同）
    _resolve_ssl_cert()

    try:
        return args.func(args)
    except SystemExit:
        raise
    except Exception as e:
        _log(f"FATAL: unhandled exception: {e}")
        traceback.print_exc(file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
