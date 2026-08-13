import os
import sys
import time

# ── 连接配置 ───────────────────────────────────────────────
os.environ.setdefault("E2B_API_URL", "http://218.245.96.148:30030")
os.environ.setdefault("E2B_API_KEY", "e2b_000000000")
os.environ.setdefault("CUBE_TEMPLATE_ID", "tpl-cb6b9dcbf5784a1487a75971")
os.environ.setdefault("SSL_CERT_FILE", r"resource/certs/e2b_CA.pem")

from e2b_code_interpreter import Sandbox

# 命令通道 patch（复用 sandbox_diag.py 收敛实现）:
#   - apply_ssl_patch : 跳过自建部署的证书校验(CERTIFICATE_VERIFY_FAILED)
#   - patch_envd_url  : SDK 默认 {port}-{id}.cube.app hostname 路由走不通,
#                       内部 nginx 不识别 SNI → RST errno=54;
#                       必须重写为 path-based https://218.245.96.148:443/sandbox/{id}/49983
from sandbox_diag import apply_ssl_patch, patch_envd_url

# ── 网络:0.3.1 上 allow_out 是白名单(未列出的目标=丢弃)──
#   10.0.0.0/8      —— CoreDNS(kube-dns 10.96.x.x)做域名解析
#   218.0.0.0/8     —— 控制面 + 沙箱 API(218.245.96.148)
#   202.38.95.0/24  —— USTC PyPI 镜像(mirrors.ustc.edu.cn 解析到 202.38.95.110)
# 需要访问别的公网服务,把对应 IP/网段加进 ALLOW_OUT 即可。
ALLOW_OUT = ["10.0.0.0/8", "218.0.0.0/8", "202.38.95.0/24"]

# pip 源:USTC 镜像(国内,快)。http + --trusted-host 避开任何 HTTPS 证书问题。
INDEX_URL = "http://mirrors.ustc.edu.cn/pypi/simple"
TRUSTED_HOST = "mirrors.ustc.edu.cn"

PACKAGE = "uv"
SANDBOX_TIMEOUT = 1800  # 沙箱存活秒数(成功后保留沙箱,留足验证时间)
PIP_TIMEOUT = 300       # pip install 命令超时秒数


def build_pip_cmd() -> str:
    # sudo 装到系统路径 /usr/local/bin(非登录 shell 也在 PATH 上),
    # 避免"装到 ~/.local/bin 只有登录 shell 找得到"的坑。
    return (f"sudo pip install {PACKAGE} --break-system-packages "
            f"-i {INDEX_URL} --trusted-host {TRUSTED_HOST}")


def _el(t0: float) -> float:
    return time.monotonic() - t0


def run_cmd(sb, cmd: str, timeout: int, label: str = "cmd") -> dict:
    """流式执行单条命令:实时打印带时间戳的 stdout/stderr。
    超时/异常被杀时,也能拿到被杀前已流式输出的部分日志。"""
    print(f"\n┌─[{label}] timeout={timeout}s")
    print(f"│ $ {cmd}")
    t0 = time.monotonic()
    out_buf, err_buf = [], []

    def on_stdout(s: str):
        out_buf.append(s)
        for ln in s.splitlines():
            print(f"│ [{_el(t0):6.2f}s|out] {ln}", flush=True)

    def on_stderr(s: str):
        err_buf.append(s)
        for ln in s.splitlines():
            print(f"│ [{_el(t0):6.2f}s|err] {ln}", flush=True)

    exit_code = None
    err_type = err_msg = ""
    try:
        proc = sb.commands.run(cmd, background=True, timeout=timeout)
        result = proc.wait(on_stdout=on_stdout, on_stderr=on_stderr)
        exit_code = result.exit_code
    except Exception as e:
        err_type = type(e).__name__
        err_msg = str(e)
        for attr in ("exit_code", "stdout", "stderr", "error"):
            val = getattr(e, attr, None)
            if val:
                if attr in ("stdout", "stderr") and val not in ("".join(out_buf), "".join(err_buf)):
                    print(f"│ [{_el(t0):6.2f}s|exc.{attr}] {val}", flush=True)
                elif attr == "error":
                    print(f"│ [{_el(t0):6.2f}s|exc.error] {val}", flush=True)

    elapsed = time.monotonic() - t0
    print(f"└─[{label}] exit={exit_code} elapsed={elapsed:.2f}s"
          + (f"  exc={err_type}" if err_type else "")
          + (f": {err_msg[:200]}" if err_msg else ""))
    return {
        "exit_code": exit_code,
        "stdout": "".join(out_buf),
        "stderr": "".join(err_buf),
        "elapsed": elapsed,
        "error_type": err_type,
        "error_msg": err_msg,
    }


def main() -> int:
    print("=" * 64)
    print("CubeSandbox 0.3.1  pip install uv 测试")
    print("=" * 64)
    print(f"  E2B_API_URL   = {os.environ['E2B_API_URL']}")
    print(f"  template      = {os.environ['CUBE_TEMPLATE_ID']}")
    print(f"  allow_out     = {ALLOW_OUT}")
    print(f"  pip index     = {INDEX_URL}")
    print(f"  trusted-host  = {TRUSTED_HOST}")
    print(f"  package       = {PACKAGE}")
    print("-" * 64)

    sb = None
    keep_alive = False
    try:
        print("\n[*] 创建沙箱 ...")
        t0 = time.monotonic()
        apply_ssl_patch()
        sb = Sandbox.create(
            template=os.environ["CUBE_TEMPLATE_ID"],
            timeout=SANDBOX_TIMEOUT,
            network={"allow_out": ALLOW_OUT},
        )
        print(f"[+] 沙箱就绪: {sb.sandbox_id}  (create {time.monotonic()-t0:.2f}s)")
        patch_envd_url(sb)

        # 1) 网络诊断:先确认 USTC 镜像在白名单内可达
        run_cmd(sb,
                f"echo '--- curl {INDEX_URL}/uv/ ---'; "
                f"curl -sS --connect-timeout 10 --max-time 20 "
                f"-o /dev/null -w 'http=%{{http_code}} time=%{{time_total}}s\\n' "
                f"{INDEX_URL}/uv/ 2>&1; echo \"curl_rc=$?\"",
                30, "curl USTC mirror")

        # 2) pip install uv
        r = run_cmd(sb, build_pip_cmd(), PIP_TIMEOUT, "pip install uv")
        if r["exit_code"] != 0:
            print(f"\n[!] pip install 失败 exit={r['exit_code']} elapsed={r['elapsed']:.1f}s")
            return 1

        # 3) 验证:登录 shell + 非登录 shell(模拟你进沙箱的交互环境)
        v = run_cmd(sb, "uv --version", 30, "uv --version (login)")
        n = run_cmd(sb, "bash -c 'uv --version'", 30, "uv --version (non-login)")
        ok = v["exit_code"] == 0 and n["exit_code"] == 0
        verify_msg = (v["stdout"].strip() or n["stdout"].strip())
        if not ok:
            s = run_cmd(sb, f"pip show {PACKAGE}", 30, "pip show uv")
            if s["exit_code"] == 0:
                ok, verify_msg = True, "pip show 确认已安装(uv 可能不在 PATH)"

        if ok:
            keep_alive = True
            print("\n" + "=" * 64)
            print("[+] 安装成功,沙箱已保留(不删除),可进去验证")
            print(f"    sandbox_id = {sb.sandbox_id}")
            print(f"    存活 TTL   = {SANDBOX_TIMEOUT}s(到点自动回收;可 sb.set_timeout() 续期)")
            print(f"    验证结果   = {verify_msg}")
            print("-" * 64)
            print("重连沙箱(另开终端/脚本,保持同样的 E2B_API_URL / SSL_CERT_FILE env):")
            print("    from e2b_code_interpreter import Sandbox")
            print(f'    sb = Sandbox.connect("{sb.sandbox_id}")')
            print('    print(sb.commands.run("uv --version").stdout)')
            print('    print(sb.commands.run("pip show uv").stdout)')
            print("    # 验证完手动回收:  sb.kill()")
            print("=" * 64)
            return 0
        print("\n[!] 安装后验证失败,沙箱将被清理")
        return 1
    except Exception as e:
        print(f"\n[!] 异常: {type(e).__name__}: {e}")
        return 2
    finally:
        if sb is not None and not keep_alive:
            try:
                sb.kill()
                print("\n[*] 沙箱已 kill")
            except Exception as e:
                print(f"\n[*] kill warning: {type(e).__name__}: {e}")


if __name__ == "__main__":
    sys.exit(main())