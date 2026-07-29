#!/usr/bin/env python3
"""
E2B 沙箱运维脚本 — 列出/清理/远程执行远端 E2B 沙箱。

用法:
  # 列出所有运行中的沙箱
  python3 scripts/e2b/sandbox_list.py list

  # 列出所有沙箱（含已暂停）
  python3 scripts/e2b/sandbox_list.py list --all

  # 列出并显示详细信息（创建时间、模板、状态等）
  python3 scripts/e2b/sandbox_list.py list --verbose

  # 清理指定沙箱
  python3 scripts/e2b/sandbox_list.py kill <sandbox_id>

  # 清理所有运行中的沙箱（需要 --force 确认）
  python3 scripts/e2b/sandbox_list.py kill-all --force

  # 清理超时沙箱（运行超过 N 分钟）
  python3 scripts/e2b/sandbox_list.py prune --older-than 30

  # 在沙箱中远程执行命令（-- 后的内容作为命令原样传递）
  python3 scripts/e2b/sandbox_list.py run <sandbox_id> -- ls -la /home/user/
  python3 scripts/e2b/sandbox_list.py run <sandbox_id> -- cat /home/user/output.txt

环境变量:
  E2B_API_URL         E2B API 地址（默认 http://218.245.96.148:30030）
  E2B_API_KEY         E2B API 密钥（默认 e2b_000000000）
  SSL_CERT_FILE       CA 证书路径（自签名证书时必填）
"""

import argparse
import os
import sys
from datetime import datetime, timezone, timedelta
from pathlib import Path

# 确保 e2b_code_interpreter 已安装
try:
    from e2b_code_interpreter import Sandbox
    from e2b.sandbox.sandbox_api import SandboxQuery, SandboxState
except ImportError:
    print("[e2b_list] ERROR: e2b_code_interpreter not installed")
    print("  pip install e2b_code_interpreter")
    sys.exit(1)


# ── 默认配置 ──────────────────────────────────────────────────────────
_DEFAULT_API_URL = "http://218.245.96.148:30030"
_DEFAULT_API_KEY = "e2b_000000000"
_CA_CERT_PATH = str(Path(__file__).resolve().parent.parent.parent / "resource" / "certs" / "e2b_CA.pem")


def _apply_ssl_patch() -> None:
    """SSLPatch: 内部 E2B 部署使用 IP 直连，跳过证书验证。"""
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


# 内部 E2B 部署用 path-based 反向代理（cubeproxy），SDK 构造的 hostname 路由
# （https://{port}-{id}.cube.app）不可达，需重写为 path-based 路由
_DEFAULT_ENVD_PORT = 49983
_SANDBOX_TARGET_HOST = "https://218.245.96.148:443"


def _patch_envd_url(sb) -> None:
    """将 sandbox 的 envd_api_url 从 hostname 路由重写为 path-based 路由。

    原 SDK 默认: https://{port}-{id}.cube.app
    重写后:     https://218.245.96.148:443/sandbox/{id}/{port}

    内部部署使用 cubeproxy 做反向代理，server_name _; 不接受 *.cube.app SNI。
    """
    path = f"{_SANDBOX_TARGET_HOST}/sandbox/{sb.sandbox_id}/{_DEFAULT_ENVD_PORT}"
    # 私有 name-mangled 属性（SandboxBase.__envd_api_url）
    try:
        sb._SandboxBase__envd_api_url = path
    except AttributeError:
        pass
    sb._envd_api_url = path
    # 子客户端模块持有自己的 URL 副本
    for attr in ("_filesystem", "_commands", "_pty"):
        mod = getattr(sb, attr, None)
        if mod is not None and hasattr(mod, "_envd_api_url"):
            mod._envd_api_url = path


def _setup_env() -> None:
    """设置 E2B SDK 环境变量。"""
    os.environ.setdefault("E2B_API_URL", _DEFAULT_API_URL)
    os.environ.setdefault("E2B_API_KEY", _DEFAULT_API_KEY)
    # CA 证书：如果文件存在且未设置 SSL_CERT_FILE，自动配置
    ca_path = os.environ.get("SSL_CERT_FILE", "")
    if not ca_path and Path(_CA_CERT_PATH).is_file():
        os.environ["SSL_CERT_FILE"] = _CA_CERT_PATH
        ca_path = _CA_CERT_PATH

    print(f"[e2b_list] API: {os.environ.get('E2B_API_URL')}")
    if ca_path:
        print(f"[e2b_list] CA:  {ca_path}")


def _fmt_time(dt: datetime | None) -> str:
    """格式化时间戳为可读字符串。"""
    if dt is None:
        return "N/A"
    if dt.tzinfo is None:
        dt = dt.replace(tzinfo=timezone.utc)
    now = datetime.now(timezone.utc)
    delta = now - dt
    if delta < timedelta(minutes=1):
        return f"{int(delta.total_seconds())}s ago"
    elif delta < timedelta(hours=1):
        return f"{int(delta.total_seconds() / 60)}m ago"
    elif delta < timedelta(days=1):
        return f"{int(delta.total_seconds() / 3600)}h ago"
    else:
        return f"{int(delta.days)}d ago"


def _connect_sandbox(sandbox_id: str) -> Sandbox:
    """重连已有 E2B 沙箱。用 Sandbox.connect（SDK v2 的权威重连接口）。"""
    sb = Sandbox.connect(sandbox_id)
    # 重写 envd URL：内部部署用 path-based 反向代理，SDK 构造的 hostname 路由不可达
    _patch_envd_url(sb)
    return sb


def _kill_sandbox(sandbox_id: str, envd_version: str = "0.2.0") -> None:
    """重连并销毁沙箱，幂等。"""
    s = _connect_sandbox(sandbox_id, envd_version)
    s.kill()


def _sandbox_age(sandbox_info: dict) -> timedelta | None:
    """从沙箱信息中提取运行时长。"""
    created = None
    for key in ("created_at", "started_at", "timestamp"):
        val = sandbox_info.get(key)
        if val:
            try:
                if isinstance(val, (int, float)):
                    created = datetime.fromtimestamp(val, tz=timezone.utc)
                elif isinstance(val, str):
                    created = datetime.fromisoformat(val.replace("Z", "+00:00"))
                if created:
                    break
            except (ValueError, TypeError):
                continue
    if created:
        return datetime.now(timezone.utc) - created
    return None


# ═══════════════════════════════════════════════════════════════════════
# 子命令
# ═══════════════════════════════════════════════════════════════════════


def cmd_list(args: argparse.Namespace) -> int:
    """列出 E2B 沙箱。"""
    _setup_env()

    # 构造查询条件
    if args.all:
        sandbox_query = None  # 全部状态
    elif args.state:
        state_map = {
            "running": SandboxState.RUNNING,
            "paused": SandboxState.PAUSED,
        }
        states = [state_map[s] for s in args.state if s in state_map]
        sandbox_query = SandboxQuery(state=states) if states else None
    else:
        # 默认只查运行中
        sandbox_query = SandboxQuery(state=[SandboxState.RUNNING])

    try:
        paginator = Sandbox.list(query=sandbox_query)
        sandboxes = paginator.next_items()
    except Exception as e:
        print(f"[e2b_list] ERROR: failed to list sandboxes: {e}")
        return 1

    total = len(sandboxes)

    if total == 0:
        print(f"[e2b_list] No sandboxes found.")
        return 0

    print(f"[e2b_list] Found {total} sandbox(es):")
    print()

    for i, sb in enumerate(sandboxes, 1):
        sandbox_id = getattr(sb, "sandbox_id", getattr(sb, "id", "unknown"))
        state = getattr(sb, "state", "unknown")
        template = getattr(sb, "template", "N/A")
        created = getattr(sb, "created_at", getattr(sb, "started_at", None))

        # 尝试获取详细信息
        info = {}
        try:
            info = sb.get_info() if hasattr(sb, "get_info") else {}
        except Exception:
            pass

        age = _sandbox_age(info) if info else None
        age_str = f" ({_fmt_time(created)})" if created else ""
        age_detail = f", age={int(age.total_seconds() / 60)}m" if age else ""

        print(f"  #{i}")
        print(f"    ID:      {sandbox_id}")
        print(f"    State:   {state}{age_str}")
        if age:
            print(f"    Age:     {int(age.total_seconds())}s ({int(age.total_seconds() / 60)}m)")
        print(f"    Template: {template}")

        if args.verbose and info:
            for key, val in info.items():
                if key not in ("sandbox_id", "id", "state", "template"):
                    print(f"    {key}: {val}")
        print()

    return 0


def cmd_kill(args: argparse.Namespace) -> int:
    """销毁指定沙箱。"""
    _setup_env()

    sandbox_id = args.sandbox_id
    print(f"[e2b_list] Killing sandbox: {sandbox_id}")

    try:
        _kill_sandbox(sandbox_id)
        print(f"[e2b_list] ✅ Sandbox {sandbox_id} killed successfully.")
        return 0
    except Exception as e:
        msg = str(e).lower()
        if "404" in msg or "not found" in msg or "already" in msg:
            print(f"[e2b_list] ⚠️  Sandbox {sandbox_id} already gone.")
            return 0
        print(f"[e2b_list] ERROR: failed to kill sandbox {sandbox_id}: {e}")
        return 1


def cmd_kill_all(args: argparse.Namespace) -> int:
    """销毁所有运行中的沙箱（需要 --force 确认）。"""
    _setup_env()

    if not args.force:
        print("[e2b_list] ⚠️  This will kill ALL running sandboxes.")
        print("  Use --force to confirm.")
        return 1

    try:
        paginator = Sandbox.list(query=SandboxQuery(state=[SandboxState.RUNNING]))
        sandboxes = list(paginator.next_items())
    except Exception as e:
        print(f"[e2b_list] ERROR: failed to list sandboxes: {e}")
        return 1

    if not sandboxes:
        print("[e2b_list] No running sandboxes to kill.")
        return 0

    print(f"[e2b_list] Killing {len(sandboxes)} sandbox(es)...")
    failed = 0
    for sb in sandboxes:
        sandbox_id = getattr(sb, "sandbox_id", getattr(sb, "id", "unknown"))
        envd_ver = getattr(sb, "envd_version", "0.2.0")
        try:
            _kill_sandbox(sandbox_id, envd_ver)
            print(f"  ✅ {sandbox_id}")
        except Exception as e:
            print(f"  ❌ {sandbox_id}: {e}")
            failed += 1

    if failed:
        print(f"[e2b_list] Done: {len(sandboxes) - failed} killed, {failed} failed.")
        return 1
    else:
        print(f"[e2b_list] Done: all {len(sandboxes)} sandbox(es) killed.")
        return 0


def cmd_prune(args: argparse.Namespace) -> int:
    """清理运行超过指定时间的沙箱。"""
    _setup_env()

    older_minutes = args.older_than
    cutoff = datetime.now(timezone.utc) - timedelta(minutes=older_minutes)

    try:
        paginator = Sandbox.list(query=SandboxQuery(state=[SandboxState.RUNNING]))
        sandboxes = list(paginator.next_items())
    except Exception as e:
        print(f"[e2b_list] ERROR: failed to list sandboxes: {e}")
        return 1

    if not sandboxes:
        print("[e2b_list] No running sandboxes to check.")
        return 0

    stale = []
    for sb in sandboxes:
        sandbox_id = getattr(sb, "sandbox_id", getattr(sb, "id", "unknown"))
        info = {}
        try:
            info = sb.get_info() if hasattr(sb, "get_info") else {}
        except Exception:
            pass

        age = _sandbox_age(info)
        if age and age > timedelta(minutes=older_minutes):
            stale.append((sandbox_id, age, info))

    if not stale:
        print(f"[e2b_list] No sandboxes older than {older_minutes}m found.")
        return 0

    print(f"[e2b_list] Found {len(stale)} sandbox(es) older than {older_minutes}m:")
    for sandbox_id, age, _ in stale:
        print(f"  - {sandbox_id} ({int(age.total_seconds() / 60)}m)")

    if not args.force:
        print()
        print("  Use --force to kill them.")
        return 1

    print()
    failed = 0
    for sandbox_id, age, info in stale:
        envd_ver = info.get("envd_version", "0.2.0") if isinstance(info, dict) else "0.2.0"
        try:
            _kill_sandbox(sandbox_id, envd_ver)
            print(f"  ✅ {sandbox_id} killed ({int(age.total_seconds() / 60)}m old)")
        except Exception as e:
            print(f"  ❌ {sandbox_id}: {e}")
            failed += 1

    if failed:
        print(f"[e2b_list] Done: {len(stale) - failed} killed, {failed} failed.")
        return 1
    else:
        print(f"[e2b_list] Done: all {len(stale)} stale sandbox(es) killed.")
        return 0


def cmd_run(args: argparse.Namespace) -> int:
    """在指定沙箱中执行远程命令。"""
    _setup_env()
    _apply_ssl_patch()

    sandbox_id = args.sandbox_id
    command = args.cmd
    timeout = getattr(args, "timeout", 30)

    try:
        sb = _connect_sandbox(sandbox_id)
    except Exception as e:
        print(f"[e2b_run] ERROR: failed to connect to sandbox {sandbox_id}: {e}")
        return 1

    print(f"[e2b_run] 📡 {sandbox_id}$ {command}")
    print()

    try:
        result = sb.commands.run(command, timeout=timeout)
    except Exception as e:
        print(f"[e2b_run] ERROR: command execution failed: {e}")
        return 1

    if result.stdout:
        print(result.stdout.rstrip("\n"))
    if result.stderr:
        print(result.stderr.rstrip("\n"), file=sys.stderr)

    if result.error:
        print(f"\n[e2b_run] ❌ error: {result.error}", file=sys.stderr)
        return 1

    print(f"\n[e2b_run] ✅ exit_code={result.exit_code}")
    return 0


# ═══════════════════════════════════════════════════════════════════════
# CLI 入口
# ═══════════════════════════════════════════════════════════════════════


def main() -> int:
    parser = argparse.ArgumentParser(
        description="E2B 沙箱运维工具 — 查询和清理远端沙箱",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    sub = parser.add_subparsers(dest="command", help="子命令")

    # list
    p_list = sub.add_parser("list", help="列出沙箱")
    p_list.add_argument("--all", action="store_true", help="列出所有状态（含暂停）")
    p_list.add_argument("--state", nargs="+", choices=["running", "paused"],
                        help="按状态过滤")
    p_list.add_argument("--verbose", "-v", action="store_true", help="显示详细信息")

    # kill
    p_kill = sub.add_parser("kill", help="销毁指定沙箱")
    p_kill.add_argument("sandbox_id", help="沙箱 ID")

    # kill-all
    p_kill_all = sub.add_parser("kill-all", help="销毁所有运行中的沙箱")
    p_kill_all.add_argument("--force", "-f", action="store_true", help="确认执行")

    # prune
    p_prune = sub.add_parser("prune", help="清理超时沙箱")
    p_prune.add_argument("--older-than", type=int, default=30,
                         help="超时阈值（分钟），默认 30")
    p_prune.add_argument("--force", "-f", action="store_true", help="确认执行")

    # run
    p_run = sub.add_parser("run", help="在沙箱中执行远程命令")
    p_run.add_argument("sandbox_id", help="沙箱 ID")
    p_run.add_argument("cmd", nargs=argparse.REMAINDER, help="要执行的命令")
    p_run.add_argument("--timeout", type=int, default=30,
                       help="命令超时秒数（默认 30）")
    p_run.add_argument("--envd-version", default="0.2.0",
                       help="envd 版本（默认 0.2.0）")

    args = parser.parse_args()

    if args.command == "list":
        return cmd_list(args)
    elif args.command == "kill":
        return cmd_kill(args)
    elif args.command == "kill-all":
        return cmd_kill_all(args)
    elif args.command == "prune":
        return cmd_prune(args)
    elif args.command == "run":
        # argparse.REMAINDER 返回 list，拼接为字符串
        cmd = " ".join(args.cmd) if args.cmd else ""
        if not cmd:
            print("[e2b_run] ERROR: no command specified.")
            return 1
        args.cmd = cmd
        return cmd_run(args)
    else:
        parser.print_help()
        return 1


if __name__ == "__main__":
    sys.exit(main())