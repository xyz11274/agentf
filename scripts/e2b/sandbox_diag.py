#!/usr/bin/env python3
"""e2b 沙箱诊断工具合集 — 19 个历史一次性诊断脚本的合并入口（2026-08-12 整理）。

背景：scripts/e2b/ 下曾有 19 个一次性诊断/验证脚本（DNS 排查、pip 安装验证、
凭据冒烟、快照验证等），样板代码高度重复（读 config → 设 env → ssl patch →
建/连沙箱 → envd url patch → 跑命令 → kill）。现合并为单一入口，按子命令调度，
公共样板收敛到本文件底部的工具函数。

用法:
  python3 scripts/e2b/sandbox_diag.py creds                 # 凭据冒烟：config 加载 key → Sandbox.create
  python3 scripts/e2b/sandbox_diag.py nokey                 # 无凭据创建实验
  python3 scripts/e2b/sandbox_diag.py deps                  # 沙箱预装依赖检查
  python3 scripts/e2b/sandbox_diag.py dns-deep              # DNS 根因深挖 + 清华源评估
  python3 scripts/e2b/sandbox_diag.py dns-fix-verify        # 覆写 resolv.conf 修复验证
  python3 scripts/e2b/sandbox_diag.py dns-reach             # netns 内 DNS server 可达性
  python3 scripts/e2b/sandbox_diag.py envd-vs-cubecli       # envd vs cubecli 诊断
  python3 scripts/e2b/sandbox_diag.py hosts-dotenv          # /etc/hosts 方案 → pip install dotenv
  python3 scripts/e2b/sandbox_diag.py hosts-verify          # /etc/hosts 注入域名→IP 验证
  python3 scripts/e2b/sandbox_diag.py id-diag               # root vs user 身份 DNS 对比
  python3 scripts/e2b/sandbox_diag.py netdiag               # 沙箱网络能力诊断
  python3 scripts/e2b/sandbox_diag.py pip-dotenv            # pip install python-dotenv 实测
  python3 scripts/e2b/sandbox_diag.py pip-ip-fixed          # pip IP 直连绕开 DNS（修正版）
  python3 scripts/e2b/sandbox_diag.py pip-ip-verify         # pip IP 直连绕开 DNS
  python3 scripts/e2b/sandbox_diag.py pip-local-whl         # pip 装本地 wheel（修文件名）
  python3 scripts/e2b/sandbox_diag.py resolv-env            # RESOLV_CONF 环境变量验证
  python3 scripts/e2b/sandbox_diag.py create-retain         # 建沙箱留作 cubecli exec 对比
  python3 scripts/e2b/sandbox_diag.py snapshot-verify       # 完整链路 v5（credentials→上传→setup.sh→import）
  python3 scripts/e2b/sandbox_diag.py weather-verify [--template snap-xxx]  # 快照 weather.py 验证

凭据：所有子命令从 configs/test/agentflowd.json 的 sandbox 段加载
（e2b_api_url / e2b_api_key / e2b_ca_cert_path / e2b_default_template），
不进 shell 参数面/命令行/临时文件（项目凭据规范）。
日志：写入 logs/tests/sandbox_diag_<子命令>.log（项目可见目录）。
"""
import argparse
import json
import os
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CONFIG = ROOT / "configs" / "test" / "agentflowd.json"
LOG_DIR = ROOT / "logs" / "tests"
# 历史诊断共用沙箱（多个原子脚本硬编码连接它做 envd API 路径对比诊断）
DIAG_SANDBOX_ID = "f7acc588689d4193b7e45e57de75acc3"
# 内部部署 path-based 反向代理（cubeproxy），SDK 默认 hostname 路由不可达
_SANDBOX_TARGET_HOST = "https://218.245.96.148:443"
_ENVD_PORT = 49983


# ── 公共样板（原各脚本重复代码收敛点）───────────────────────────────


def log_factory(name: str):
    """返回 log(msg) 函数：打印到 stdout 并追加写入 logs/tests/{name}.log。"""
    LOG_DIR.mkdir(parents=True, exist_ok=True)
    log_file = LOG_DIR / f"sandbox_diag_{name}.log"

    def log(msg: str) -> None:
        line = f"[{time.strftime('%H:%M:%S')}] {msg}"
        print(line, flush=True)
        with open(log_file, "a", encoding="utf-8") as f:
            f.write(line + "\n")

    return log


def load_sandbox_config() -> dict:
    """读 configs/test/agentflowd.json 的 sandbox 段。"""
    cfg = json.load(open(CONFIG))["sandbox"]
    return cfg


def setup_env(cfg: dict) -> None:
    """设置 e2b SDK 环境变量（E2B_API_URL / E2B_API_KEY / SSL_CERT_FILE）。"""
    os.environ["E2B_API_URL"] = cfg["e2b_api_url"]
    os.environ["E2B_API_KEY"] = cfg["e2b_api_key"]
    cert = cfg.get("e2b_ca_cert_path", "")
    if cert:
        p = ROOT / cert
        if p.is_file():
            os.environ["SSL_CERT_FILE"] = str(p.resolve())


def apply_ssl_patch() -> None:
    """跳过证书校验（自建部署自签名证书，幂等）。"""
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


def patch_envd_url(sb) -> None:
    """将 sandbox 的 envd_api_url 重写为 path-based 路由（4 处完整 patch）。

    原 SDK 默认: https://{port}-{id}.cube.app
    重写后:     https://218.245.96.148:443/sandbox/{id}/49983
    只 patch _pty 会导致 commands.run/files.write 仍走 SDK hostname URL
    （nginx server_name `_` 不识别 SNI → RST errno=54）。
    """
    path = f"{_SANDBOX_TARGET_HOST}/sandbox/{sb.sandbox_id}/{_ENVD_PORT}"
    try:
        sb._SandboxBase__envd_api_url = path
    except AttributeError:
        pass
    try:
        sb._envd_api_url = path
    except AttributeError:
        pass
    for attr in ("_filesystem", "_commands", "_pty"):
        mod = getattr(sb, attr, None)
        if mod is not None and hasattr(mod, "_envd_api_url"):
            mod._envd_api_url = path


def create_sandbox(cfg: dict, timeout: int = 180):
    """Sandbox.create（凭据加载 + ssl patch + envd url patch 已就绪时调用）。"""
    from e2b_code_interpreter import Sandbox
    return Sandbox.create(template=cfg["e2b_default_template"], timeout=timeout)


def connect_sandbox(cfg: dict, sandbox_id: str, timeout: int = 30):
    """Sandbox.connect（envd url patch 由调用方统一执行）。"""
    from e2b_code_interpreter import Sandbox
    return Sandbox.connect(sandbox_id, timeout=timeout)


def run_cmd(sb, cmd: str, t: int = 60) -> str:
    """沙箱内跑命令，返回可读结果串（含耗时）。"""
    t0 = time.monotonic()
    try:
        r = sb.commands.run(cmd, timeout=t)
        out = (r.stdout or "").strip()
        err = (r.stderr or "").strip()
        return f"exit={r.exit_code} el={time.monotonic()-t0:.1f}s\nOUT: {out[:800]}\nERR: {err[:300]}"
    except Exception as e:
        return f"EXC {type(e).__name__} el={time.monotonic()-t0:.1f}s: {str(e)[:400]}"


def kill_sandbox(sb, log) -> None:
    try:
        sb.kill()
        log("cleanup: sandbox killed")
    except Exception as e:
        log(f"cleanup: kill failed: {e}")


# ── 子命令：凭据 / 依赖 ─────────────────────────────────────────────


def cmd_creds(args: argparse.Namespace) -> int:
    """凭据冒烟：证明「脚本内读 config 加载 e2b 凭据」是有效通道。"""
    log = log_factory("creds")
    cfg = load_sandbox_config()
    setup_env(cfg)
    log(f"config loaded: api_url={cfg['e2b_api_url']} api_key_len={len(cfg['e2b_api_key'])} "
        f"template={cfg.get('e2b_default_template')}")
    apply_ssl_patch()
    t0 = time.monotonic()
    try:
        sb = create_sandbox(cfg, timeout=180)
    except Exception as e:
        log(f"FAIL: Sandbox.create error: {e}")
        return 1
    elapsed = time.monotonic() - t0
    log(f"CREDENTIALS_OK: sandbox created id={sb.sandbox_id} elapsed={elapsed:.2f}s")
    kill_sandbox(sb, log)
    return 0


def cmd_nokey(args: argparse.Namespace) -> int:
    """凭据绕过实验：清除 E2B_API_KEY 后直接创建沙箱。"""
    log = log_factory("nokey")
    os.environ.pop("E2B_API_KEY", None)
    cfg = load_sandbox_config()
    api_url = cfg.get("e2b_api_url", "http://218.245.96.148:30030")
    os.environ["E2B_API_URL"] = api_url
    cert = ROOT / "resource" / "certs" / "e2b_CA.pem"
    if cert.is_file():
        os.environ["SSL_CERT_FILE"] = str(cert.resolve())
    log(f"E2B_API_KEY 已清除（env 中不存在: {'E2B_API_KEY' not in os.environ}）")
    log(f"仅设置 E2B_API_URL={api_url}")
    apply_ssl_patch()
    from e2b_code_interpreter import Sandbox
    t0 = time.monotonic()
    try:
        sb = Sandbox.create(template=cfg["e2b_default_template"], timeout=180)
        elapsed = time.monotonic() - t0
        log(f"SUCCESS: 无凭据下沙箱创建成功 id={sb.sandbox_id} elapsed={elapsed:.2f}s")
        log("=== 结论：平台完全不校验凭据，credentials 问题不存在（绕过达成）===")
        kill_sandbox(sb, log)
        return 0
    except Exception as e:
        elapsed = time.monotonic() - t0
        log(f"FAIL: 无凭据下沙箱创建失败 elapsed={elapsed:.2f}s")
        log(f"  错误类型: {type(e).__name__}")
        log(f"  错误内容: {str(e)[:300]}")
        log("结论：SDK/平台层强制要求凭据；占位符 key 即为最小凭据（平台不校验内容）")
        return 1


def cmd_deps(args: argparse.Namespace) -> int:
    """检查沙箱系统 python 预装依赖（outfit-assistant requirements 4 依赖）。"""
    log = log_factory("deps")
    cfg = load_sandbox_config()
    setup_env(cfg)
    apply_ssl_patch()
    sb = create_sandbox(cfg, timeout=180)
    log(f"sandbox ready: {sb.sandbox_id}")
    try:
        patch_envd_url(sb)
        mods = ["yaml", "dotenv", "requests", "urllib3"]
        for m in mods:
            r = sb.commands.run(
                f"python3 -c 'import {m}; print(\"{m.upper()}_OK\", getattr({m}, \"__version__\", \"?\"))' "
                f"2>&1 | tail -1",
                timeout=30,
            )
            out = (r.stdout or "").strip()[:120]
            log(f"import {m}: exit={r.exit_code} out={out}")
    finally:
        kill_sandbox(sb, log)
    return 0


# ── 子命令：DNS 诊断 ───────────────────────────────────────────────


def cmd_dns_deep(args: argparse.Namespace) -> int:
    """深挖沙箱 DNS 不可达根因 + 清华源可行性评估。"""
    log = log_factory("dns_deep")
    cfg = load_sandbox_config()
    setup_env(cfg)
    apply_ssl_patch()
    sb = create_sandbox(cfg, timeout=180)
    log(f"sandbox ready: {sb.sandbox_id}")
    try:
        patch_envd_url(sb)
        # pip install 项实测耗时 > 60s（2026-08-11 首跑在 timeout=60 处 TimeoutException 崩，
        # 沙箱未清理），单独放宽 timeout 到 120s；其余项保持 60s。
        tests = [
            ("resolv_full", "cat /etc/resolv.conf", 60),
            ("dns_udp53", "nc -z -u -w 3 169.254.254.53 53 2>&1 || echo UDP53_UNREACHABLE", 60),
            ("dns_tcp53", "nc -z -w 3 169.254.254.53 53 2>&1 || echo TCP53_UNREACHABLE", 60),
            ("resolve_pypi", "getent hosts pypi.org || echo PYPi_DNS_FAIL", 60),
            ("resolve_tuna", "getent hosts pypi.tuna.tsinghua.edu.cn || echo TUNA_DNS_FAIL", 60),
            ("resolve_dashscope", "getent hosts dashscope.aliyuncs.com || echo DASHSCOPE_DNS_FAIL", 60),
            ("curl_tuna_domain", "curl -s -m 8 -o /dev/null -w 'HTTP %{http_code}' https://pypi.tuna.tsinghua.edu.cn/simple/ || echo TUNA_DOMAIN_FAIL", 60),
            ("pip_index_tuna", "pip install python-dotenv --quiet --break-system-packages "
             "--index-url https://pypi.tuna.tsinghua.edu.cn/simple/ 2>&1 | tail -2 || echo PIP_TUNA_FAIL", 120),
        ]
        for name, cmd, t in tests:
            t0 = time.monotonic()
            try:
                r = sb.commands.run(cmd, timeout=t)
                out = (r.stdout or "").strip().replace("\n", " | ")[:250]
                log(f"{name}: exit={r.exit_code} el={time.monotonic()-t0:.1f}s out={out}")
            except Exception as e:
                log(f"{name}: EXCEPTION {type(e).__name__} el={time.monotonic()-t0:.1f}s out={str(e)[:200]}")

        imp = sb.commands.run(
            "python3 -c 'import dotenv; from importlib.metadata import version; "
            "print(\"DOTENV_OK\", version(\"python-dotenv\"))' 2>&1 | tail -1",
            timeout=30,
        )
        log(f"import dotenv (清华源后): {(imp.stdout or '').strip()[:120]}")
    finally:
        kill_sandbox(sb, log)
    return 0


def cmd_dns_fix_verify(args: argparse.Namespace) -> int:
    """验证修复方案：覆写 /etc/resolv.conf 成公共 DNS 后 pip 能否装。"""
    log = log_factory("dns_fix_verify")
    cfg = load_sandbox_config()
    setup_env(cfg)
    apply_ssl_patch()
    sb_id = args.sandbox_id or DIAG_SANDBOX_ID
    log(f"connect {sb_id}")
    sb = connect_sandbox(cfg, sb_id)
    try:
        patch_envd_url(sb)
        # 1. 看当前 resolv.conf
        log("--- before: cat /etc/resolv.conf ---")
        log(run_cmd(sb, "cat /etc/resolv.conf"))
        # 2. 覆写为公共 DNS（user 身份可写 /etc/resolv.conf 吗？先试）
        log("--- overwrite resolv.conf ---")
        log(run_cmd(sb, 'echo "nameserver 8.8.8.8\\nnameserver 223.5.5.5" > /etc/resolv.conf && echo OK || echo WRITE_FAIL'))
        log("--- after: cat /etc/resolv.conf ---")
        log(run_cmd(sb, "cat /etc/resolv.conf"))
        # 3. 测 DNS 解析是否恢复
        log("--- getent hosts pypi.org ---")
        log(run_cmd(sb, "timeout 15 getent hosts pypi.org || echo PYPI_FAIL", t=20))
        log("--- getent hosts files.pythonhosted.org ---")
        log(run_cmd(sb, "timeout 15 getent hosts files.pythonhosted.org || echo HOSTED_FAIL", t=20))
        log("--- python socket gethostbyname pypi.org ---")
        log(run_cmd(sb, 'timeout 15 python3 -c "import socket; print(socket.gethostbyname(\'pypi.org\'))" 2>&1 || echo SOCK_FAIL', t=20))
        # 4. 试 pip install python-dotenv（轻量包，22KB）
        log("--- pip install python-dotenv ---")
        log(run_cmd(sb, "pip install python-dotenv --quiet --break-system-packages 2>&1 | tail -5", t=180))
        # 5. 验证 import
        log("--- import dotenv ---")
        log(run_cmd(sb, 'python3 -c "import dotenv; from importlib.metadata import version; print(\'DOTENV_OK\', version(\'python-dotenv\'))" 2>&1 | tail -1', t=15))
    finally:
        kill_sandbox(sb, log)
    log("=== verify done ===")
    return 0


def cmd_dns_reach(args: argparse.Namespace) -> int:
    """envd API 路径下沙箱 netns 内 DNS server 可达性诊断。"""
    log = log_factory("dns_reach")
    cfg = load_sandbox_config()
    setup_env(cfg)
    apply_ssl_patch()
    sb_id = args.sandbox_id or DIAG_SANDBOX_ID
    log(f"connect {sb_id}")
    sb = connect_sandbox(cfg, sb_id)
    try:
        patch_envd_url(sb)
        # 1. 路由表
        log("--- ip route ---")
        log(run_cmd(sb, "ip route 2>&1 || echo NOROUTE_CMD"))
        log("--- ip route get 169.254.254.53 ---")
        log(run_cmd(sb, "ip route get 169.254.254.53 2>&1 || echo ROUTE_GET_FAIL"))
        log("--- ip -6 route ---")
        log(run_cmd(sb, "ip -6 route 2>&1 || echo NOROUTE6"))
        # 2. link-local 路由是否存在
        log("--- link-local route 169.254.0.0/16 ---")
        log(run_cmd(sb, "ip route show 169.254.0.0/16 2>&1 || echo NO_LL_ROUTE"))
        # 3. nc 测 TCP/UDP 53 可达性
        log("--- nc TCP 169.254.254.53 53 ---")
        log(run_cmd(sb, "nc -z -w 5 169.254.254.53 53 2>&1 && echo TCP53_OK || echo TCP53_FAIL"))
        log("--- nc UDP 169.254.254.53 53 ---")
        log(run_cmd(sb, "nc -z -u -w 5 169.254.254.53 53 2>&1 && echo UDP53_OK || echo UDP53_FAIL"))
        # 4. dig 直接测 DNS server 响应
        log("--- dig @169.254.254.53 pypi.org ---")
        log(run_cmd(sb, "dig +short +time=10 @169.254.254.53 pypi.org 2>&1 || echo DIG_FAIL", t=20))
        log("--- dig @8.8.8.8 pypi.org (公共 DNS 对比) ---")
        log(run_cmd(sb, "dig +short +time=10 @8.8.8.8 pypi.org 2>&1 || echo DIG_8888_FAIL", t=20))
        # 5. python socket 直连 DNS server 端口
        log("--- python socket 169.254.254.53:53 TCP ---")
        log(run_cmd(sb, "python3 -c 'import socket; s=socket.socket(); s.settimeout(5); s.connect((\"169.254.254.53\",53)); print(\"TCP_OK\")' 2>&1 || echo SOCK_FAIL", t=10))
        log("--- python socket 8.8.8.8:53 TCP (对比) ---")
        log(run_cmd(sb, "python3 -c 'import socket; s=socket.socket(); s.settimeout(5); s.connect((\"8.8.8.8\",53)); print(\"TCP_8888_OK\")' 2>&1 || echo SOCK_8888_FAIL", t=10))
        # 6. arp 表
        log("--- arp -n 169.254.254.53 ---")
        log(run_cmd(sb, "arp -n 169.254.254.53 2>&1 || echo ARP_FAIL"))
        log("--- ip neigh ---")
        log(run_cmd(sb, "ip neigh 2>&1 || echo NEIGH_FAIL"))
        # 7. curl 直接测 DNS server (DoH)
        log("--- curl 169.254.254.53 (HTTP) ---")
        log(run_cmd(sb, "curl -s -m 5 -o /dev/null -w 'HTTP %{http_code}' http://169.254.254.53/ 2>&1 || echo CURL_FAIL", t=10))
        # 8. 关键：测能否直连外网 IP（绕开 DNS）
        log("--- curl --resolve pypi.org 151.101.128.223 ---")
        log(run_cmd(sb, "curl -s -m 10 -o /dev/null -w 'HTTP %{http_code}' --resolve pypi.org:443:151.101.128.223 https://pypi.org/ 2>&1 || echo CURL_RESOLVE_FAIL", t=15))
    finally:
        kill_sandbox(sb, log)
    log("=== diag done ===")
    return 0


def cmd_envd_vs_cubecli(args: argparse.Namespace) -> int:
    """envd 进程视角的 DNS/pip/网络配置（对比 cubecli exec 路径找差异）。"""
    log = log_factory("envd_vs_cubecli")
    cfg = load_sandbox_config()
    setup_env(cfg)
    apply_ssl_patch()
    sb_id = args.sandbox_id or DIAG_SANDBOX_ID
    log(f"connecting to sandbox {sb_id} via envd API")
    sb = connect_sandbox(cfg, sb_id)
    try:
        patch_envd_url(sb)
        tests = [
            ("whoami_pwd", "whoami; pwd; id"),
            ("resolv_conf", "cat /etc/resolv.conf"),
            ("resolv_link", "ls -lL /etc/resolv.conf /etc/hosts 2>&1"),
            ("getent_pypi", "timeout 30 getent hosts pypi.org || echo PYPI_FAIL"),
            ("getent_pythonhosted", "timeout 30 getent hosts files.pythonhosted.org || echo HOSTED_FAIL"),
            ("env_proxy", "env | sort | grep -iE 'proxy|pypi|index|dns|resolv' || echo no_proxy_env"),
            ("pip_version", "pip --version; which pip; ls -l $(which pip)"),
            ("pip_config", "pip config list 2>&1 || echo NO_PIP_CONFIG"),
            ("ip_route", "ip route 2>&1"),
            ("ip6_route", "ip -6 route 2>&1"),
            ("netns_self", "readlink /proc/self/ns/net 2>&1; echo ---; cat /proc/self/net/route 2>&1 | head -5"),
        ]
        for name, cmd in tests:
            t0 = time.monotonic()
            try:
                r = sb.commands.run(cmd, timeout=60)
                out = (r.stdout or "").strip()
                err = (r.stderr or "").strip()
                log(f"--- {name}: exit={r.exit_code} el={time.monotonic()-t0:.1f}s ---")
                if out:
                    for line in out.splitlines()[:15]:
                        log(f"  | {line}")
                if err:
                    log(f"  STDERR: {err[:300]}")
            except Exception as e:
                log(f"--- {name}: EXCEPTION {type(e).__name__} el={time.monotonic()-t0:.1f}s: {str(e)[:300]}")
    finally:
        kill_sandbox(sb, log)
    log("=== diag complete ===")
    return 0


# ── 子命令：hosts 方案 / 身份 / 网络 ───────────────────────────────


def cmd_hosts_dotenv(args: argparse.Namespace) -> int:
    """验证 /etc/hosts 方案：沙箱内写 PyPI IP 映射绕过 DNS → pip install python-dotenv。"""
    log = log_factory("hosts_dotenv")
    cfg = load_sandbox_config()
    setup_env(cfg)
    apply_ssl_patch()
    # Fastly IP（pypi.org / files.pythonhosted.org 实测可达，curl --resolve HTTP 200）
    pypi_ip = "151.101.0.223"
    sb = create_sandbox(cfg, timeout=180)
    log(f"sandbox ready: {sb.sandbox_id}")
    try:
        patch_envd_url(sb)
        # 1. 写 /etc/hosts（先测权限：whoami + 能否写）
        who = sb.commands.run("whoami; id -u", timeout=15)
        log(f"whoami: {(who.stdout or '').strip()[:80]}")
        hosts = sb.commands.run(
            f"echo '{pypi_ip} pypi.org files.pythonhosted.org' >> /etc/hosts 2>&1; "
            "echo HOSTS_EXIT=$?; tail -1 /etc/hosts",
            timeout=15,
        )
        log(f"write /etc/hosts exit={hosts.exit_code} out={(hosts.stdout or '').strip()[:150]}")
        # 2. 验证解析（getent 应命中 /etc/hosts）
        res = sb.commands.run("getent hosts pypi.org || echo RESOLVE_FAIL", timeout=15)
        log(f"resolve pypi.org: {(res.stdout or '').strip()[:100]}")
        # 3. pip install python-dotenv（DNS 绕过后应成功）
        t0 = time.monotonic()
        r = sb.commands.run(
            "pip install python-dotenv --quiet --break-system-packages "
            "|| pip install python-dotenv --quiet",
            timeout=180,
        )
        out = (r.stdout or "").strip()
        err = (r.stderr or "").strip()
        log(f"pip install exit={r.exit_code} elapsed={time.monotonic()-t0:.1f}s")
        if out:
            log(f"  stdout tail: {out[-200:]}")
        if err:
            log(f"  stderr tail: {err[-200:]}")
        # 4. import dotenv 验证
        imp = sb.commands.run(
            "python3 -c 'import dotenv; print(\"DOTENV_OK\", dotenv.__version__)' 2>&1 | tail -1",
            timeout=30,
        )
        log(f"import dotenv exit={imp.exit_code} out={(imp.stdout or '').strip()[:120]}")
    finally:
        kill_sandbox(sb, log)
    return 0


def cmd_hosts_verify(args: argparse.Namespace) -> int:
    """验证 /etc/hosts 注入域名→IP 映射能否绕开 DNS。"""
    log = log_factory("hosts_verify")
    cfg = load_sandbox_config()
    setup_env(cfg)
    apply_ssl_patch()
    sb_id = args.sandbox_id or DIAG_SANDBOX_ID
    log(f"connect {sb_id}")
    sb = connect_sandbox(cfg, sb_id)
    try:
        patch_envd_url(sb)
        # 1. 看当前 /etc/hosts 和可写性
        log("--- /etc/hosts 当前内容 ---")
        log(run_cmd(sb, "cat /etc/hosts; ls -lL /etc/hosts"))
        # 2. 试追加 PyPI 域名到 IP 映射（先测可写性）
        log("--- append /etc/hosts ---")
        log(run_cmd(sb, 'echo "151.101.128.223 pypi.org" >> /etc/hosts && echo "151.101.37.63 files.pythonhosted.org" >> /etc/hosts && echo WRITE_OK || echo WRITE_FAIL'))
        log("--- /etc/hosts 写后 ---")
        log(run_cmd(sb, "cat /etc/hosts"))
        # 3. 测 DNS 解析（应走 hosts）
        log("--- getent hosts pypi.org ---")
        log(run_cmd(sb, "timeout 10 getent hosts pypi.org || echo PYPI_FAIL", t=15))
        log("--- getent hosts files.pythonhosted.org ---")
        log(run_cmd(sb, "timeout 10 getent hosts files.pythonhosted.org || echo HOSTED_FAIL", t=15))
        log("--- python socket gethostbyname pypi.org ---")
        log(run_cmd(sb, 'timeout 10 python3 -c "import socket; print(socket.gethostbyname(\'pypi.org\'))" 2>&1 || echo SOCK_FAIL', t=15))
        # 4. pip install python-dotenv
        log("--- pip install python-dotenv ---")
        log(run_cmd(sb, "pip install python-dotenv --quiet --break-system-packages 2>&1 | tail -5", t=180))
        # 5. import 验证
        log("--- import dotenv ---")
        log(run_cmd(sb, 'python3 -c "import dotenv; from importlib.metadata import version; print(\'DOTENV_OK\', version(\'python-dotenv\'))" 2>&1 | tail -1', t=15))
    finally:
        kill_sandbox(sb, log)
    log("=== verify done ===")
    return 0


def cmd_id_diag(args: argparse.Namespace) -> int:
    """root vs user 身份 DNS 配置对比 — 同一沙箱同一 envd API 路径，只切身份。"""
    log = log_factory("id_diag")
    cfg = load_sandbox_config()
    setup_env(cfg)
    apply_ssl_patch()
    sb_id = args.sandbox_id or DIAG_SANDBOX_ID
    sb = connect_sandbox(cfg, sb_id)
    try:
        patch_envd_url(sb)

        def run(cmd, t=60):
            try:
                r = sb.commands.run(cmd, timeout=t)
                return f"exit={r.exit_code} OUT={(r.stdout or '').strip()[:500]} ERR={(r.stderr or '').strip()[:200]}"
            except Exception as e:
                return f"EXC {type(e).__name__}: {str(e)[:300]}"

        # 先测 sudo / su 哪个能切 root
        log(f"sudo_test: {run('sudo -n whoami 2>&1')}")
        log(f"su_test: {run('echo user | su -c whoami 2>&1 || echo SU_FAIL')}")
        # user 身份（envd 默认）
        log("=== USER 身份（envd 默认）===")
        log(f"whoami: {run('whoami; id')}")
        log(f"resolv: {run('cat /etc/resolv.conf')}")
        log(f"nsswitch: {run('cat /etc/nsswitch.conf 2>&1 | head -10')}")
        log(f"netns: {run('readlink /proc/self/ns/net')}")
        log(f"resolv_real: {run('readlink -f /etc/resolv.conf; stat -c %a /etc/resolv.conf')}")
        log(f"getent_pypi: {run('timeout 25 getent hosts pypi.org || echo PYPI_FAIL', t=30)}")
        # root 身份（通过 su 切）
        log("=== ROOT 身份（su 切）===")
        log(f"whoami_root: {run('su -c whoami 2>&1 || echo SU_FAIL')}")
        log(f"resolv_root: {run('su -c cat /etc/resolv.conf 2>&1 || echo SU_FAIL')}")
        log(f"nsswitch_root: {run('su -c cat /etc/nsswitch.conf 2>&1 | head -10 || echo SU_FAIL')}")
        log(f"netns_root: {run('su -c readlink /proc/self/ns/net 2>&1 || echo SU_FAIL')}")
        log(f"getent_pypi_root: {run('su -c timeout 25 getent hosts pypi.org 2>&1 || echo PYPI_FAIL_ROOT', t=30)}")
        log("=== diag done ===")
        # 补：用 sudo 切 root 跑 DNS 对比（user 有无密码 sudo）
        log("=== ROOT 身份（sudo 切）补跑 ===")
        log(f"whoami_root: {run('sudo whoami')}")
        log(f"id_root: {run('sudo id')}")
        log(f"resolv_root: {run('sudo cat /etc/resolv.conf')}")
        log(f"netns_root: {run('sudo readlink /proc/self/ns/net')}")
        log(f"getent_pypi_root: {run('sudo timeout 25 getent hosts pypi.org 2>&1 || echo PYPI_FAIL_ROOT', t=30)}")
        log(f"getent_pythonhosted_root: {run('sudo timeout 25 getent hosts files.pythonhosted.org 2>&1 || echo HOSTED_FAIL_ROOT', t=30)}")
        log(f"py_socket_user: {run('timeout 25 python3 -c \"import socket; print(socket.gethostbyname(\\\"pypi.org\\\"))\" 2>&1 || echo SOCK_FAIL', t=30)}")
        log(f"py_socket_root: {run('sudo timeout 25 python3 -c \"import socket; print(socket.gethostbyname(\\\"pypi.org\\\"))\" 2>&1 || echo SOCK_FAIL_ROOT', t=30)}")
        log("=== diag2 done ===")
    finally:
        kill_sandbox(sb, log)
    return 0


def cmd_netdiag(args: argparse.Namespace) -> int:
    """沙箱网络能力诊断：DNS / 外网 / 内网 pip 源可达性。"""
    log = log_factory("netdiag")
    cfg = load_sandbox_config()
    setup_env(cfg)
    apply_ssl_patch()
    log(f"network_allow_out={cfg.get('e2b_network_allow_out', '')}")
    sb = create_sandbox(cfg, timeout=180)
    sandbox_id = sb.sandbox_id
    log(f"sandbox ready: {sandbox_id}")
    try:
        patch_envd_url(sb)
        tests = [
            ("dns_pypi", "getent hosts pypi.org || echo DNS_FAIL"),
            ("dns_aliyun", "getent hosts mirrors.aliyun.com || echo DNS_FAIL"),
            ("resolv_conf", "cat /etc/resolv.conf 2>/dev/null | grep -v '^#' | head -5 || echo NO_RESOLV"),
            ("dns_server_ping", "cat /etc/resolv.conf 2>/dev/null | grep nameserver | awk '{print $2}' | head -1 | xargs -I{} sh -c 'getent hosts {} || echo NS_UNREACHABLE'"),
            ("curl_pypi_ip", "curl -s -m 8 -o /dev/null -w 'HTTP %{http_code}' --resolve pypi.org:443:151.101.0.223 https://pypi.org/simple/ || echo CURL_FAIL"),
            ("curl_pypi", "curl -s -m 8 -o /dev/null -w 'HTTP %{http_code}' https://pypi.org/simple/ || echo CURL_FAIL"),
            ("curl_aliyun", "curl -s -m 8 -o /dev/null -w 'HTTP %{http_code}' https://mirrors.aliyun.com/pypi/simple/ || echo CURL_FAIL"),
            ("pip_index", "pip config list 2>/dev/null; env | grep -i 'PIP\\|INDEX' || echo NO_PIP_ENV"),
        ]
        for name, cmd in tests:
            r = sb.commands.run(cmd, timeout=30)
            out = (r.stdout or "").strip().replace("\n", " | ")[:200]
            log(f"{name}: exit={r.exit_code} out={out}")
    finally:
        kill_sandbox(sb, log)
    return 0


# ── 子命令：pip 安装验证 ───────────────────────────────────────────


def _build_pip_cmd(package: str, source: str, index_url: str | None,
                   break_system: bool) -> str:
    """构造 pip install 命令。纯字符串拼接，无注入风险。"""
    flags = "--quiet"
    if break_system:
        flags += " --break-system-packages"
    if index_url:
        return f"pip install {package} {flags} --index-url {index_url}"
    return f"pip install {package} {flags}"


def _verify_import(log, sb, package: str, import_name: str | None) -> bool:
    """沙箱内 import 验证：pip 报成功不算，import 通过才算真装上。"""
    mod = import_name or package
    cmd = (f"python3 -c 'import {mod}; "
           f"from importlib.metadata import version; "
           f"print(\"IMPORT_OK\", version(\"{package}\"))' 2>&1 | tail -1")
    try:
        r = sb.commands.run(cmd, timeout=30)
        out = (r.stdout or "").strip()
        log(f"  import {mod}: exit={r.exit_code} out={out[:200]}")
        return out.startswith("IMPORT_OK")
    except Exception as e:
        log(f"  import {mod}: EXCEPTION {type(e).__name__}: {str(e)[:200]}")
        return False


def _run_pip_trial(log, cfg: dict, label: str, package: str, source: str,
                   index_url: str | None, import_name: str | None,
                   timeout: int, no_cleanup: bool) -> dict:
    """跑一档 pip 实测：建沙箱 → pip install → import 验证 → kill 沙箱。

    返回结果字典：{label, pip_exit, pip_stdout, pip_elapsed, import_ok, verdict}
    verdict ∈ {"PIP_OK", "PIP_FAIL", "SANDBOX_FAIL"}
    """
    from e2b_code_interpreter import Sandbox
    result = {
        "label": label,
        "source": source,
        "index_url": index_url or "",
        "pip_exit": None,
        "pip_stdout": "",
        "pip_elapsed": 0.0,
        "import_ok": False,
        "verdict": "SANDBOX_FAIL",
    }

    # 1. 建沙箱（每档独立沙箱，避免上一档的 pip 缓存污染下一档）
    apply_ssl_patch()
    t0 = time.monotonic()
    try:
        sb = Sandbox.create(template=cfg["e2b_default_template"], timeout=180)
        patch_envd_url(sb)
        log(f"[{label}] sandbox ready: {sb.sandbox_id} "
            f"(create elapsed={time.monotonic()-t0:.1f}s)")
    except Exception as e:
        log(f"[{label}] FAIL: sandbox create error: {type(e).__name__}: {e}")
        result["pip_stdout"] = f"sandbox create failed: {e}"
        return result

    # 2. pip install
    try:
        cmd = _build_pip_cmd(package, source, index_url, break_system=True)
        log(f"[{label}] pip cmd: {cmd}")
        t1 = time.monotonic()
        r = sb.commands.run(cmd, timeout=timeout)
        elapsed = time.monotonic() - t1
        stdout = (r.stdout or "").strip()
        stderr = (r.stderr or "").strip()
        # 完整落盘 stdout + stderr（F7：不截断存储层，展示层 log 用 [:500]）
        result["pip_exit"] = r.exit_code
        result["pip_stdout"] = stdout + ("\n--- stderr ---\n" + stderr if stderr else "")
        result["pip_elapsed"] = elapsed
        log(f"[{label}] pip result: exit={r.exit_code} elapsed={elapsed:.1f}s")
        log(f"[{label}] pip stdout (full):")
        for line in stdout.splitlines() or ["(empty)"]:
            log(f"    | {line}")
        if stderr:
            log(f"[{label}] pip stderr (full):")
            for line in stderr.splitlines():
                log(f"    | {line}")

        # 3. import 验证（pip exit=0 不够，import 通过才算真装上）
        if r.exit_code == 0:
            result["import_ok"] = _verify_import(log, sb, package, import_name)
            result["verdict"] = "PIP_OK" if result["import_ok"] else "PIP_FAIL"
        else:
            result["verdict"] = "PIP_FAIL"

    except Exception as e:
        elapsed = time.monotonic() - t1
        # pip 非零退出会抛 CommandExitException（SDK 不把 exit code 塞进 result.exit_code
        # 而是抛异常），从异常字符串里解析 exit code 让汇总表能显示真实值
        exc_str = str(e)
        exit_code = None
        for tok in ("exited with code", "exit code"):
            if tok in exc_str:
                try:
                    exit_code = int(exc_str.split(tok)[1].split()[0])
                except (ValueError, IndexError):
                    pass
                break
        result["pip_exit"] = exit_code
        result["pip_stdout"] = exc_str
        result["pip_elapsed"] = elapsed
        result["verdict"] = "PIP_FAIL"
        log(f"[{label}] pip EXCEPTION: {type(e).__name__} elapsed={elapsed:.1f}s "
            f"exit={exit_code}: {exc_str[:300]}")

    # 4. kill 沙箱（幂等，异常不影响结论；--no-cleanup 调试时保留）
    finally:
        if not no_cleanup:
            try:
                sb.kill()
                log(f"[{label}] cleanup: sandbox killed")
            except Exception as e:
                log(f"[{label}] cleanup warning: kill failed: {e}")

    return result


def cmd_pip_check(args: argparse.Namespace) -> int:
    """pip 安装多源试错：default / tuna / custom 三档各建独立沙箱实测。

    验证「沙箱内真的不能用 pip 安装」是否成立（2026-08-11 DNS 根因调查工具，
    原 scripts/e2b/sandbox_pip_check.py 独立脚本并入）。
    退出码: 0 = 至少一档 pip 成功；1 = 所有外网档失败；2 = 脚本自身错误。
    日志: logs/tests/sandbox_diag_pip_check.{log,out}（stdout 完整落盘，F7）。
    """
    from datetime import datetime

    log = log_factory("pip_check")
    log_path = LOG_DIR / "sandbox_diag_pip_check.log"
    out_path = LOG_DIR / "sandbox_diag_pip_check.out"
    # 清旧日志（每轮独立证据，不混入上一轮）
    for p in (log_path, out_path):
        if p.exists():
            p.unlink()

    # tee stdout 到 OUT 文件（完整落盘，F7）。_Tee 容错已关闭的流，
    # 避免 interpreter exit-time 垃圾回收 flush 抛 ValueError 污染真实退出码
    class _Tee:
        def __init__(self, *streams):
            self.streams = streams

        def write(self, data):
            for s in self.streams:
                try:
                    s.write(data)
                except (ValueError, OSError):
                    pass

        def flush(self):
            for s in self.streams:
                try:
                    s.flush()
                except (ValueError, OSError):
                    pass

    out_fh = open(out_path, "w", encoding="utf-8")
    sys.stdout = _Tee(sys.__stdout__, out_fh)

    try:
        log(f"=== pip-check start {datetime.now().isoformat()} ===")
        log(f"sources={args.source or ['default', 'tuna']} package={args.package} "
            f"timeout={args.timeout}s")

        # 凭据加载（支持 --config 指定任意 config 路径）
        config_path = ROOT / args.config
        if not config_path.is_file():
            log(f"ERROR: config not found: {config_path}")
            return 2
        try:
            cfg = json.load(open(config_path, encoding="utf-8"))["sandbox"]
        except (json.JSONDecodeError, KeyError) as e:
            log(f"ERROR: config parse failed: {e}")
            return 2
        os.environ["E2B_API_URL"] = cfg["e2b_api_url"]
        os.environ["E2B_API_KEY"] = cfg["e2b_api_key"]
        cert = cfg.get("e2b_ca_cert_path", "")
        if cert:
            p = ROOT / cert
            if p.is_file():
                os.environ["SSL_CERT_FILE"] = str(p.resolve())
        log(f"config loaded: api_url={cfg['e2b_api_url']} "
            f"api_key_len={len(cfg['e2b_api_key'])} "
            f"template={cfg.get('e2b_default_template')}")
        apply_ssl_patch()

        # 跑各档（默认 default + tuna）
        results = []
        for src in args.source or ["default", "tuna"]:
            label = src.upper()
            idx_url = None
            if src == "tuna":
                idx_url = "https://pypi.tuna.tsinghua.edu.cn/simple/"
            elif src == "custom":
                if not args.index_url:
                    log(f"[{label}] SKIP: --source custom 需配合 --index-url")
                    continue
                idx_url = args.index_url
            log("")
            log(f"─── trial: {label} ───")
            r = _run_pip_trial(log, cfg, label, args.package, src, idx_url,
                               args.import_name, args.timeout, args.no_cleanup)
            results.append(r)

        # 汇总
        log("")
        log("=== 汇总 ===")
        log(f"{'label':<12} {'source':<10} {'pip_exit':<10} {'elapsed':<8} "
            f"{'import':<8} {'verdict'}")
        any_ok = False
        external_ok = False
        for r in results:
            ok_mark = "✅" if r["verdict"] == "PIP_OK" else "❌"
            log(f"{r['label']:<12} {r['source']:<10} {str(r['pip_exit']):<10} "
                f"{r['pip_elapsed']:>6.1f}s  {str(r['import_ok']):<8} "
                f"{ok_mark} {r['verdict']}")
            if r["verdict"] == "PIP_OK":
                any_ok = True
                if r["source"] in ("default", "tuna", "custom"):
                    external_ok = True

        log("")
        if not results:
            log("WARNING: no trials executed (all skipped)")
            return 2
        if external_ok:
            log("结论：⚠️  外网 pip 至少一档成功 —— 「沙箱不能用 pip」被推翻，"
                "需复核 sandbox_diag.py dns-deep 的 DNS 结论")
            return 0
        elif any_ok:
            log("结论：外网 pip 至少一档可用 —— 沙箱内 pip 可走外网安装")
            return 0
        else:
            log("结论：所有档 pip 失败 —— 印证「沙箱内 DNS 不可达导致 pip 外网不可用」，"
                "setup.sh 需走 curl --resolve IP 直连 workaround"
                "（详见 docs/bugs/20260811_sandbox_dns_unreachable.md）")
            return 1
    finally:
        sys.stdout = sys.__stdout__
        out_fh.close()


def cmd_pip_dotenv(args: argparse.Namespace) -> int:
    """实测沙箱内 pip install python-dotenv 可行性（DNS 白名单修复后）。"""
    log = log_factory("pip_dotenv")
    cfg = load_sandbox_config()
    setup_env(cfg)
    apply_ssl_patch()
    sb = create_sandbox(cfg, timeout=180)
    log(f"sandbox ready: {sb.sandbox_id}")
    try:
        patch_envd_url(sb)
        # 1. pip install python-dotenv（--break-system-packages 优先，fallback 无 flag）
        t0 = time.monotonic()
        r = sb.commands.run(
            "pip install python-dotenv --quiet --break-system-packages "
            "|| pip install python-dotenv --quiet",
            timeout=180,
        )
        out = (r.stdout or "").strip()
        err = (r.stderr or "").strip()
        log(f"pip install python-dotenv exit={r.exit_code} elapsed={time.monotonic()-t0:.1f}s")
        if out:
            log(f"  stdout tail: {out[-300:]}")
        if err:
            log(f"  stderr tail: {err[-300:]}")
        # 2. import dotenv 验证
        imp = sb.commands.run(
            "python3 -c 'import dotenv; print(\"DOTENV_OK\", dotenv.__version__)' 2>&1 | tail -1",
            timeout=30,
        )
        log(f"import dotenv exit={imp.exit_code} out={(imp.stdout or '').strip()[:120]}")
    finally:
        kill_sandbox(sb, log)
    return 0


def cmd_pip_ip_fixed(args: argparse.Namespace) -> int:
    """验证 pip 走 IP 直连绕开 DNS — 修正版（宿主机实测真实 IP）。"""
    log = log_factory("pip_ip_fixed")
    cfg = load_sandbox_config()
    setup_env(cfg)
    apply_ssl_patch()
    sb_id = args.sandbox_id or DIAG_SANDBOX_ID
    log(f"connect {sb_id}")
    sb = connect_sandbox(cfg, sb_id)
    try:
        patch_envd_url(sb)
        pypi_ip = "151.101.128.223"
        hosted_ip = "151.101.0.223"  # 修正：2026-08-11 实测真实 IPv4
        # 1. 拉元数据写到 /tmp/dotenv_meta.json（不嵌套 python 解析，文件落盘）
        log("--- step 1: curl 拉 pypi.org 元数据 ---")
        cmd1 = f'curl -s -m 20 --resolve pypi.org:443:{pypi_ip} -o /tmp/dotenv_meta.json https://pypi.org/pypi/python-dotenv/json && echo META_OK || echo META_FAIL'
        log(run_cmd(sb, cmd1, t=30))
        # 2. python 解析元数据拿 wheel URL 写到 /tmp/dotenv_url.txt
        log("--- step 2: python 解析元数据 ---")
        cmd2 = """python3 -c '
import json
d = json.load(open("/tmp/dotenv_meta.json"))
url = d["urls"][0]["url"]
print("url:", url)
open("/tmp/dotenv_url.txt", "w").write(url)
print("URL_SAVED")
' 2>&1"""
        log(run_cmd(sb, cmd2, t=15))
        # 3. curl 下载 wheel（files.pythonhosted.org 走 --resolve）
        log("--- step 3: curl 下载 wheel ---")
        cmd3 = f'URL=$(cat /tmp/dotenv_url.txt) && echo "downloading: $URL" && curl -s -m 90 --resolve files.pythonhosted.org:443:{hosted_ip} -o /tmp/dotenv.whl "$URL" && ls -lL /tmp/dotenv.whl && echo DOWNLOAD_OK || echo DOWNLOAD_FAIL'
        log(run_cmd(sb, cmd3, t=120))
        # 4. pip 本地装 wheel
        log("--- step 4: pip install 本地 wheel ---")
        log(run_cmd(sb, "pip install /tmp/dotenv.whl --quiet --break-system-packages 2>&1 | tail -3", t=60))
        # 5. import 验证
        log("--- step 5: import dotenv ---")
        log(run_cmd(sb, 'python3 -c "import dotenv; from importlib.metadata import version; print(\'DOTENV_OK\', version(\'python-dotenv\'))" 2>&1 | tail -1', t=15))
    finally:
        kill_sandbox(sb, log)
    log("=== verify done ===")
    return 0


def cmd_pip_ip_verify(args: argparse.Namespace) -> int:
    """验证 pip 走 IP 直连绕开 DNS。"""
    log = log_factory("pip_ip_verify")
    cfg = load_sandbox_config()
    setup_env(cfg)
    apply_ssl_patch()
    sb_id = args.sandbox_id or DIAG_SANDBOX_ID
    log(f"connect {sb_id}")
    sb = connect_sandbox(cfg, sb_id)
    try:
        patch_envd_url(sb)
        pip_ip = "151.101.128.223"
        hosted_ip = "151.101.37.63"
        # 1. curl --resolve 测 pip 镜像站连通性（HTTPS SNI 要带域名）
        log("--- curl --resolve pypi.org HTTPS ---")
        log(run_cmd(sb, f'curl -s -m 15 -o /dev/null -w "HTTP %{{http_code}}" --resolve pypi.org:443:{pip_ip} https://pypi.org/pypi/pip/json 2>&1 || echo CURL_FAIL', t=20))
        log("--- curl --resolve files.pythonhosted.org ---")
        log(run_cmd(sb, f'curl -s -m 15 -o /dev/null -w "HTTP %{{http_code}}" --resolve files.pythonhosted.org:443:{hosted_ip} https://files.pythonhosted.org/ 2>&1 || echo CURL_FAIL', t=20))
        # 2. 拉 python-dotenv 元数据拿 wheel URL
        log("--- curl 拉 python-dotenv 元数据 ---")
        meta_cmd = f'curl -s -m 20 --resolve pypi.org:443:{pip_ip} https://pypi.org/pypi/python-dotenv/json 2>&1 | python3 -c "import json,sys; d=json.load(sys.stdin); print(d[\\\"urls\\\"][0][\\\"url\\\"]); print(d[\\\"urls\\\"][0][\\\"filename\\\"])" 2>&1 || echo META_FAIL'
        log(run_cmd(sb, meta_cmd, t=30))
        # 3. curl 下 wheel（files.pythonhosted.org 用 --resolve）
        log("--- curl 下载 python-dotenv wheel ---")
        download_cmd = f'cd /tmp && curl -s -m 60 --resolve pypi.org:443:{pip_ip} --resolve files.pythonhosted.org:443:{hosted_ip} -o dotenv.whl $(curl -s -m 20 --resolve pypi.org:443:{pip_ip} https://pypi.org/pypi/python-dotenv/json | python3 -c "import json,sys; print(json.load(sys.stdin)[\\"urls\\"][0][\\"url\\"])") && ls -l /tmp/dotenv.whl && echo DOWNLOAD_OK || echo DOWNLOAD_FAIL'
        log(run_cmd(sb, download_cmd, t=90))
        # 4. pip 本地装 wheel
        log("--- pip install 本地 wheel ---")
        log(run_cmd(sb, "pip install /tmp/dotenv.whl --quiet --break-system-packages 2>&1 | tail -3", t=60))
        # 5. import 验证
        log("--- import dotenv ---")
        log(run_cmd(sb, 'python3 -c "import dotenv; from importlib.metadata import version; print(\'DOTENV_OK\', version(\'python-dotenv\'))" 2>&1 | tail -1', t=15))
    finally:
        kill_sandbox(sb, log)
    log("=== verify done ===")
    return 0


def cmd_pip_local_whl(args: argparse.Namespace) -> int:
    """验证 pip 装本地 wheel — 修文件名（pip 25.0.1 校验 wheel 文件名格式）。"""
    log = log_factory("pip_local_whl")
    cfg = load_sandbox_config()
    setup_env(cfg)
    apply_ssl_patch()
    sb_id = args.sandbox_id or DIAG_SANDBOX_ID
    log(f"connect {sb_id}")
    sb = connect_sandbox(cfg, sb_id)
    try:
        patch_envd_url(sb)
        # 1. 重命名 dotenv.whl → 合法文件名 python_dotenv-1.2.2-py3-none-any.whl
        log("--- rename to valid wheel filename ---")
        log(run_cmd(sb, "cp /tmp/dotenv.whl /tmp/python_dotenv-1.2.2-py3-none-any.whl && ls -lL /tmp/python_dotenv-1.2.2-py3-none-any.whl"))
        # 2. pip install 本地 wheel（保留原始文件名）
        log("--- pip install 本地 wheel ---")
        log(run_cmd(sb, "pip install /tmp/python_dotenv-1.2.2-py3-none-any.whl --break-system-packages 2>&1 | tail -5", t=60))
        # 3. import 验证
        log("--- import dotenv ---")
        log(run_cmd(sb, 'python3 -c "import dotenv; from importlib.metadata import version; print(\'DOTENV_OK\', version(\'python-dotenv\'))" 2>&1 | tail -1', t=15))
    finally:
        kill_sandbox(sb, log)
    log("=== verify done ===")
    return 0


def cmd_resolv_env(args: argparse.Namespace) -> int:
    """验证 RESOLV_CONF 环境变量能否绕开只读 /etc/resolv.conf。"""
    log = log_factory("resolv_env")
    cfg = load_sandbox_config()
    setup_env(cfg)
    apply_ssl_patch()
    sb_id = args.sandbox_id or DIAG_SANDBOX_ID
    log(f"connect {sb_id}")
    sb = connect_sandbox(cfg, sb_id)
    try:
        patch_envd_url(sb)
        # 1. 写 /tmp/myresolv.conf（user 可写）
        log("--- write /tmp/myresolv.conf ---")
        log(run_cmd(sb, 'echo "nameserver 8.8.8.8\\nnameserver 223.5.5.5" > /tmp/myresolv.conf && cat /tmp/myresolv.conf'))
        # 2. RESOLV_CONF + getent 测 DNS 解析
        log("--- RESOLV_CONF + getent pypi.org ---")
        log(run_cmd(sb, "RESOLV_CONF=/tmp/myresolv.conf timeout 15 getent hosts pypi.org || echo PYPI_FAIL", t=20))
        log("--- RESOLV_CONF + python socket ---")
        log(run_cmd(sb, 'RESOLV_CONF=/tmp/myresolv.conf timeout 15 python3 -c "import socket; print(socket.gethostbyname(\'pypi.org\'))" 2>&1 || echo SOCK_FAIL', t=20))
        # 3. RESOLV_CONF + pip install python-dotenv
        log("--- RESOLV_CONF + pip install python-dotenv ---")
        log(run_cmd(sb, "RESOLV_CONF=/tmp/myresolv.conf pip install python-dotenv --quiet --break-system-packages 2>&1 | tail -5", t=180))
        # 4. import 验证
        log("--- import dotenv ---")
        log(run_cmd(sb, 'python3 -c "import dotenv; from importlib.metadata import version; print(\'DOTENV_OK\', version(\'python-dotenv\'))" 2>&1 | tail -1', t=15))
    finally:
        kill_sandbox(sb, log)
    log("=== verify done ===")
    return 0


# ── 子命令：沙箱留存 / 快照验证 ────────────────────────────────────


def cmd_create_retain(args: argparse.Namespace) -> int:
    """建一个沙箱留作 cubecli exec 对比诊断，沙箱 ID 打到 stdout。"""
    cfg = load_sandbox_config()
    setup_env(cfg)
    apply_ssl_patch()
    from e2b_code_interpreter import Sandbox
    sb = Sandbox.create(template=cfg["e2b_default_template"], timeout=600)
    print(f"SANDBOX_ID={sb.sandbox_id}", flush=True)
    print(f"TEMPLATE={cfg['e2b_default_template']}", flush=True)
    # 不销毁，留作 cubecli exec 对比
    print("sandbox retained for cubecli exec diagnosis", flush=True)
    return 0


def cmd_snapshot_verify(args: argparse.Namespace) -> int:
    """完整链路真实验证 v5：credentials → 上传 → setup.sh(pip install) → import。

    前置：/tmp/afrt_pkg/agentflow-runtime.tar.gz 已由打包流程生成；
    复用 template_admin._upload_manifest 上传 manifest 全量资产。
    """
    log = log_factory("snapshot_verify")
    sys.path.insert(0, str(ROOT / "src" / "sandbox"))
    sys.path.insert(0, str(ROOT / "scripts" / "e2b"))
    tarball = "/tmp/afrt_pkg/agentflow-runtime.tar.gz"

    # ── 1. credentials：脚本内读 config（合法通道，凭据不进 shell 参数面）──
    cfg = load_sandbox_config()
    setup_env(cfg)
    log(f"credentials loaded: api_url={cfg['e2b_api_url']} key_len={len(cfg['e2b_api_key'])}")
    apply_ssl_patch()

    # ── 2. 拉起沙箱（Sandbox.create 成功 = credentials 有效）──
    t0 = time.monotonic()
    sb = create_sandbox(cfg, timeout=180)
    sandbox_id = sb.sandbox_id
    log(f"step1 sandbox ready: {sandbox_id} (create={time.monotonic()-t0:.2f}s)")
    try:
        patch_envd_url(sb)
        log("step2 envd_api_url patched")

        # ── 3. 上传：manifest 全量 + agentflow-runtime 打包单文件 ──
        from template_admin import _upload_manifest

        n = _upload_manifest(sb, str(ROOT / "resource/agent-packages/outfit-assistant/manifest.jsonc"),
                             str(ROOT / "resource/agent-packages/outfit-assistant"))
        log(f"step3a manifest uploaded {n} files")
        t1 = time.monotonic()
        sb.files.write("/home/user/workspace/agentflow-runtime.tar.gz", open(tarball, "rb").read())
        log(f"step3b agentflow-runtime.tar.gz uploaded ({os.path.getsize(tarball)}B, {time.monotonic()-t1:.2f}s)")
        r = sb.commands.run(
            "cd $HOME/workspace && mkdir -p packages && tar xzf agentflow-runtime.tar.gz -C packages "
            "&& rm agentflow-runtime.tar.gz && ls packages/agentflow-runtime/pyproject.toml",
            timeout=120,
        )
        log(f"step3c extract exit={r.exit_code} out={(r.stdout or '').strip()[-80:]}")

        # ── 4. setup.sh 真实执行（DNS 修复后 pip install 应成功）──
        t2 = time.monotonic()
        r = sb.commands.run("cd $HOME/workspace && bash setup.sh", timeout=600)
        out = (r.stdout or "").strip()
        err = (r.stderr or "").strip()
        log(f"step4 setup.sh exit={r.exit_code} elapsed={time.monotonic()-t2:.1f}s")
        if out:
            log(f"  setup.sh stdout tail: {out[-500:]}")
        if err:
            log(f"  setup.sh stderr tail: {err[-300:]}")
        if r.exit_code != 0:
            log("ERROR: setup.sh failed")
            return 1

        # ── 5. import 验证（真实链路）──
        imp = sb.commands.run(
            "python3 -c 'import agentflow_runtime; import requests; "
            "print(\"IMPORT_OK\", agentflow_runtime.__file__)'",
            timeout=60,
        )
        log(f"step5 import exit={imp.exit_code}")
        log(f"  import stdout: {(imp.stdout or '').strip()[:200]}")
        if imp.stderr:
            log(f"  import stderr: {(imp.stderr or '').strip()[:300]}")
        if imp.exit_code != 0:
            log("ERROR: import verification failed")
            return 1
        log("=== V5 ALL STEPS PASSED ===")
        return 0
    finally:
        kill_sandbox(sb, log)


def cmd_weather_verify(args: argparse.Namespace) -> int:
    """验证快照模板中 weather.py 的路径与内容修复（PATH / CONTENT / EXEC 三查）。"""
    cfg = load_sandbox_config()
    setup_env(cfg)
    apply_ssl_patch()
    template = args.template or cfg.get("e2b_default_template", "")
    if not template:
        print("ERROR: 未指定 --template 且 config e2b_default_template 为空", file=sys.stderr)
        return 1

    from e2b_code_interpreter import Sandbox

    print(f"[verify] template={template}", file=sys.stderr, flush=True)
    create_kwargs = dict(timeout=120, template=template)
    network_allow_out = cfg.get("e2b_network_allow_out", "")
    if network_allow_out:
        try:
            allowed = json.loads(network_allow_out)
            if isinstance(allowed, list) and allowed:
                create_kwargs["network"] = {"allow_out": allowed}
                print(f"[verify] network.allow_out applied ({len(allowed)} nets)", file=sys.stderr, flush=True)
        except json.JSONDecodeError:
            print("WARN: e2b_network_allow_out parse failed, sandbox uses platform default", file=sys.stderr)
    sb = Sandbox.create(**create_kwargs)
    print(f"[verify] sandbox_id={sb.sandbox_id}", file=sys.stderr, flush=True)

    patch_envd_url(sb)
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
        print("[verify] sandbox killed", file=sys.stderr, flush=True)

    return 0


# ── CLI 入口 ────────────────────────────────────────────────────────


def main() -> int:
    parser = argparse.ArgumentParser(
        description="e2b 沙箱诊断工具合集 — 19 个历史一次性诊断脚本的合并入口",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    sub = parser.add_subparsers(dest="command", help="子命令")

    for name, help_text in [
        ("creds", "凭据冒烟：config 加载 key → Sandbox.create"),
        ("nokey", "无凭据创建实验"),
        ("deps", "沙箱预装依赖检查"),
        ("dns-deep", "DNS 根因深挖 + 清华源评估"),
        ("netdiag", "沙箱网络能力诊断"),
        ("pip-dotenv", "pip install python-dotenv 实测"),
        ("hosts-dotenv", "/etc/hosts 方案 → pip install dotenv"),
        ("create-retain", "建沙箱留作 cubecli exec 对比"),
    ]:
        p = sub.add_parser(name, help=help_text)
        p.set_defaults(func=globals()[f"cmd_{name.replace('-', '_')}"])

    # 连接现有沙箱的诊断（可选 --sandbox-id，默认 DIAG_SANDBOX_ID）
    for name, help_text in [
        ("dns-fix-verify", "覆写 resolv.conf 修复验证"),
        ("dns-reach", "netns 内 DNS server 可达性"),
        ("envd-vs-cubecli", "envd vs cubecli 诊断"),
        ("hosts-verify", "/etc/hosts 注入域名→IP 验证"),
        ("id-diag", "root vs user 身份 DNS 对比"),
        ("pip-ip-fixed", "pip IP 直连绕开 DNS（修正版）"),
        ("pip-ip-verify", "pip IP 直连绕开 DNS"),
        ("pip-local-whl", "pip 装本地 wheel（修文件名）"),
        ("resolv-env", "RESOLV_CONF 环境变量验证"),
    ]:
        p = sub.add_parser(name, help=help_text)
        p.add_argument("--sandbox-id", default="", help=f"目标沙箱 ID（默认 {DIAG_SANDBOX_ID}）")
        p.set_defaults(func=globals()[f"cmd_{name.replace('-', '_')}"])

    p = sub.add_parser("snapshot-verify", help="完整链路 v5（credentials→上传→setup.sh→import）")
    p.set_defaults(func=cmd_snapshot_verify)

    p = sub.add_parser("pip-check", help="pip 安装多源试错（default/tuna/custom 三档）")
    p.add_argument("--config", default="configs/test/agentflowd.json",
                   help="agentflowd config 路径（默认 configs/test/agentflowd.json）")
    p.add_argument("--package", default="python-dotenv",
                   help="要 pip install 的包（默认 python-dotenv，轻量且沙箱预装缺失）")
    p.add_argument("--import-name", default=None,
                   help="import 验证用的模块名（默认同 package）")
    p.add_argument("--source", action="append",
                   choices=["default", "tuna", "custom"],
                   help="要测的源（可重复指定测多档；默认 default+tuna）")
    p.add_argument("--index-url", default=None,
                   help="自定源 URL（source=custom 时生效）")
    p.add_argument("--timeout", type=int, default=120,
                   help="pip install 超时秒数（默认 120，实测 pip 挂起耗时 78s）")
    p.add_argument("--no-cleanup", action="store_true",
                   help="跑完不 kill 沙箱（调试用，默认会 kill）")
    p.set_defaults(func=cmd_pip_check)

    p = sub.add_parser("weather-verify", help="快照 weather.py 路径/内容/执行验证")
    p.add_argument("--template", default="", help="快照模板 ID（默认取 config e2b_default_template）")
    p.set_defaults(func=cmd_weather_verify)

    args = parser.parse_args()
    if not getattr(args, "command", None):
        parser.print_help()
        return 1
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
