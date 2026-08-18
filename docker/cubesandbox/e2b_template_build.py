#!/usr/bin/env python3
"""e2b 自定义模板构建工具 — 一次性设置脚本（v2 预热期改造）。

用途：将 sandbox_http_server.py 预装到 e2b 模板中，并通过 set_start_cmd
预跑 HTTP server + ready_cmd 验证就绪，使 template snapshot 捕获运行中进程。
后续 Sandbox.create(template=alias) 直接加载快照（~80ms），进程已在跑。

用法：
  # 检查模板是否已存在（幂等检查，exit 0=存在 / 1=不存在）
  python3 docker/cubesandbox/e2b_template_build.py check --name agentflow-sandbox-xxx

  # 基于 base 模板构建（alias = --name）
  python3 docker/cubesandbox/e2b_template_build.py --name agentflow-sandbox-xxx

  # 基于已有模板 + 指定资源配额
  python3 docker/cubesandbox/e2b_template_build.py \\
      --name agentflow-sandbox-xxx \\
      --from-template my-custom-base \\
      --cpu 4 --memory 4096

输出：
  build 成功后打印模板 alias（= --name），可用于 Sandbox.create(template=alias)。

环境变量：
  E2B_API_KEY  必填，e2b SDK 鉴权
"""

import argparse
import os
import sys
from pathlib import Path


def _log(msg: str) -> None:
    print(f"[e2b_template_build] {msg}", file=sys.stderr)


# 共用 sandbox_patch（SSL 校验跳过——内部 e2b 部署用自签名证书 + path-based 路由）
# 与 e2b_lifecycle.py 一致：优先从 agentflow-runtime 导入，fallback 到本地定义
try:
    from agentflow_runtime.common.sandbox_patch import apply_ssl_patch as _apply_ssl_patch
except ImportError:
    def _apply_ssl_patch() -> None:
        """SSLPatch: 跳过证书验证（内部部署自签名证书）。"""
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


def _resolve_ssl_cert() -> None:
    """将 SSL_CERT_FILE 相对路径解析为绝对路径（子进程可能不继承工作目录）。"""
    cert_path = os.environ.get("SSL_CERT_FILE", "")
    if cert_path and not os.path.isabs(cert_path):
        script_dir = Path(__file__).resolve().parent
        abs_path = (script_dir / cert_path).resolve()
        if abs_path.is_file():
            os.environ["SSL_CERT_FILE"] = str(abs_path)


def _check_api_key() -> str:
    api_key = os.environ.get("E2B_API_KEY")
    if not api_key:
        print("[e2b_template_build] ERROR: E2B_API_KEY environment variable not set", file=sys.stderr)
        sys.exit(2)
    return api_key


def _server_file_path() -> Path:
    """sandbox_http_server.py 本地绝对路径（与脚本同目录 http_server/ 下）。"""
    return Path(__file__).resolve().parent / "http_server" / "sandbox_http_server.py"


def _setup_file_path() -> Path:
    """sandbox-setup.sh 本地绝对路径（与脚本同目录）。"""
    return Path(__file__).resolve().parent / "sandbox-setup.sh"


def _load_sdk():
    try:
        from e2b import Template
        from e2b.template.readycmd import wait_for_port
        return Template, wait_for_port
    except ImportError:
        _log("ERROR: e2b SDK not installed. Run: pip install e2b")
        sys.exit(1)


def check_alias(args: argparse.Namespace) -> int:
    """幂等检查：模板 templateID 是否已存在且 READY（REST 方式，不依赖 SDK）。

    CubeSandbox 适配（2026-08-09）：当前部署是 CubeSandbox，SDK `Template.alias_exists`
    端点（/templates/aliases/{alias}）在该平台不实现（404）。改用平台 REST API
    `GET /templates/{templateID}` 判定：200=存在，404=不存在，其他=SDK/网络错误。

    exit 0=存在且 READY / 1=不存在 / 2=SDK 或网络错误（无法判定）
    """
    _check_api_key()
    _resolve_ssl_cert()
    _apply_ssl_patch()  # 内部部署自签名证书，SDK 校验跳过（与 e2b_lifecycle.py 一致）
    import json
    import urllib.error
    import urllib.request

    api_url = os.environ.get("E2B_API_URL", "http://218.245.96.148:30030").rstrip("/")
    url = f"{api_url}/templates/{args.name}"
    req = urllib.request.Request(url, method="GET")
    body_text = ""
    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            status = resp.status
            body_text = resp.read().decode("utf-8", errors="replace")
    except urllib.error.HTTPError as e:
        status = e.code
        body_text = e.read().decode("utf-8", errors="replace")
    except Exception as e:
        _log(f"ERROR: REST check failed for {args.name}: {e}")
        return 2
    if status == 200:
        # 200 = 模板存在，还需验证 status==READY：PENDING（构建中）/FAILED 不可复用
        # （设计依据：sandbox-warm-template-oci.md §1.1 模板状态；GET /templates/{id} body 顶层 status 字段）
        try:
            doc = json.loads(body_text) if body_text else {}
            tpl_status = doc.get("status", "") if isinstance(doc, dict) else ""
        except json.JSONDecodeError:
            tpl_status = ""
        if tpl_status == "READY":
            _log(f"check: template={args.name} exists and READY (HTTP 200, status=READY)")
            return 0
        if tpl_status:
            # 明确非 READY（PENDING/FAILED/...）→ 不可复用，按"不存在"降级
            _log(f"check: template={args.name} exists but status={tpl_status} (not READY, cannot reuse)")
            return 1
        # status 字段缺失（结构未知）→ 回退旧行为：仅按 200 判定存在
        _log(f"check: template={args.name} exists (HTTP 200, no status field)")
        return 0
    if status == 404:
        _log(f"check: template={args.name} not exists (HTTP 404)")
        return 1
    _log(f"ERROR: unexpected HTTP {status} for {args.name}")
    return 2


def build_template(args: argparse.Namespace) -> None:
    _check_api_key()
    _resolve_ssl_cert()
    _apply_ssl_patch()  # 内部部署自签名证书，SDK 校验跳过（与 e2b_lifecycle.py 一致）
    Template, wait_for_port = _load_sdk()

    server_path = _server_file_path()
    setup_path = _setup_file_path()
    if not server_path.is_file():
        _log(f"ERROR: sandbox_http_server.py not found at {server_path}")
        return 1
    if not setup_path.is_file():
        _log(f"ERROR: sandbox-setup.sh not found at {setup_path}")
        return 1

    template_name = args.name
    _log(f"Building template: {template_name}")
    _log(f"  from_template: {args.from_template}")
    _log(f"  cpu: {args.cpu}, memory: {args.memory}MB")
    _log(f"  http_port: {args.http_port}")

    # v2 改动 #2：set_start_cmd 预跑 HTTP server + ready_cmd 验证就绪
    # 新 SDK API（e2b.template.main.TemplateBuilder）：copy / run_cmd / set_envs / set_start_cmd
    # 旧 API（add_files / add_commands / env_vars）在当前 SDK 已移除，禁止使用。
    start_cmd = (
        f"python3 /opt/sandbox/http_server/sandbox_http_server.py"
        f" --port {args.http_port} --host 0.0.0.0"
    )
    builder = (
        Template()
        .from_template(args.from_template)
        .copy(str(server_path), "/opt/sandbox/http_server/sandbox_http_server.py")
        .copy(str(setup_path), "/opt/sandbox/sandbox-setup.sh")
        .run_cmd("mkdir -p /opt/sandbox/tools && bash /opt/sandbox/sandbox-setup.sh")
        .set_envs({"AF_SANDBOX_WORKSPACE": "/workdir"})
        .set_start_cmd(start_cmd, wait_for_port(args.http_port))
    )

    # 构建模板（alias 与 --name 一致，Sandbox.create(template=alias) 按 alias 寻址）
    result = Template.build(
        builder,
        name=template_name,
        alias=template_name,
        cpu_count=args.cpu,
        memory_mb=args.memory,
    )
    _log(f"Template built successfully: alias={result.alias} id={result.template_id}")
    _log(f"Use this alias in TemplateRegistry (agentflow-sandbox-<agent_id>)")
    return 0


def register_template(args: argparse.Namespace) -> int:
    """注册已 docker build 的镜像为 CubeSandbox 模板（REST POST /templates）。

    CubeSandbox 适配（2026-08-09）：SDK `Template.build()`（POST /v3/templates）
    在该平台不实现（404）——code-defined template build 路径不可用。改走平台 REST
    `POST /templates {image, writableLayerSize, exposePort, probe, probePath}`，
    将节点本地已构建的 docker image 转为 sandbox template。body 必须 camelCase。

    HTTP server 预跑不靠 SDK set_start_cmd（Cube 不实现），改由 Dockerfile.warm 的
    COPY+RUN 在镜像构建期预置 sandbox_http_server.py，REST probe 在模板创建期
    拉起沙箱验证就绪 → snapshot 捕获运行中进程 → READY 后复用毫秒级。

    exit 0=注册并 READY / 1=参数或 HTTP 错误 / 2=轮询超时
    """
    import json
    import time
    import urllib.error
    import urllib.request

    _check_api_key()
    _resolve_ssl_cert()
    _apply_ssl_patch()

    api_url = os.environ.get("E2B_API_URL", "http://218.245.96.148:30030").rstrip("/")
    body = {
        "image": args.image,
        "writableLayerSize": args.writable_layer_size,
        "exposePort": args.http_port,
        "probe": args.http_port,
        "probePath": "/exec",
    }
    _log(f"register: POST {api_url}/templates image={args.image} port={args.http_port}")

    req = urllib.request.Request(
        f"{api_url}/templates",
        data=json.dumps(body).encode("utf-8"),
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            doc = json.loads(resp.read().decode("utf-8", errors="replace"))
            tpl_id = doc.get("templateID") or doc.get("templateId") or ""
            job_id = doc.get("jobID") or doc.get("jobId") or ""
            status = doc.get("status", "")
            _log(f"register: POST {resp.status} templateID={tpl_id} jobID={job_id} status={status}")
    except urllib.error.HTTPError as e:
        err_body = e.read().decode("utf-8", errors="replace")[:300]
        _log(f"register: POST HTTP {e.code} {err_body}")
        return 1
    except Exception as e:
        _log(f"register: POST err: {e}")
        return 1

    if not tpl_id:
        _log("register: response missing templateID, cannot poll")
        return 1

    # 轮询 GET /templates/{tpl_id} 直到 READY/FAILED
    deadline = time.monotonic() + args.timeout
    while time.monotonic() < deadline:
        time.sleep(args.poll_interval)
        req2 = urllib.request.Request(f"{api_url}/templates/{tpl_id}", method="GET")
        try:
            with urllib.request.urlopen(req2, timeout=15) as resp2:
                doc2 = json.loads(resp2.read().decode("utf-8", errors="replace"))
                s = doc2.get("status", "")
                _log(f"register: poll status={s}")
                if s == "READY":
                    print(tpl_id)
                    return 0
                if s == "FAILED":
                    err = doc2.get("lastError") or doc2.get("error") or "(no error field)"
                    _log(f"register: FAILED {err[:300]}")
                    return 2
        except urllib.error.HTTPError as e:
            _log(f"register: poll HTTP {e.code} (retrying)")
        except Exception as e:
            _log(f"register: poll err: {e}")

    _log(f"register: timeout after {args.timeout}s, template {tpl_id} not READY")
    return 2


def delete_template(args: argparse.Namespace) -> int:
    """删除平台模板/快照（REST DELETE /templates/{id}）。

    CubeSandbox 适配（2026-08-09 实测）：`DELETE /templates/{snapshotID}` → 204
    删除成功（复查 GET 404 确认）。用于 agent 级快照回收（防膨胀，设计
    sandbox-warm-template-oci.md §2.6.4）：新快照 READY 后删旧快照。

    exit 0=删除成功 / 1=参数错误 / 2=REST 错误（无法判定，保留旧快照不删——先建后删）
    """
    import urllib.error
    import urllib.request

    _check_api_key()
    _resolve_ssl_cert()
    _apply_ssl_patch()

    api_url = os.environ.get("E2B_API_URL", "http://218.245.96.148:30030").rstrip("/")
    tpl_id = args.template_id
    _log(f"delete: DELETE {api_url}/templates/{tpl_id}")

    req = urllib.request.Request(f"{api_url}/templates/{tpl_id}", method="DELETE")
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            _log(f"delete: DELETE {resp.status} (template={tpl_id})")
            # 204/200 均视为删除成功；复查存在性交给调用方（可选 GET 404 确认）
            return 0
    except urllib.error.HTTPError as e:
        body = e.read().decode("utf-8", errors="replace")[:300]
        _log(f"delete: DELETE HTTP {e.code} template={tpl_id} {body}")
        # 404 = 已不存在（幂等视为成功）；其他 = 错误
        return 0 if e.code == 404 else 2
    except Exception as e:
        _log(f"delete: err: {e}")
        return 2


def main() -> int:
    parser = argparse.ArgumentParser(
        description="e2b 自定义模板构建工具（v2：set_start_cmd 预跑 + ready_cmd 验证）"
    )
    sub = parser.add_subparsers(dest="subcommand")

    # check 子命令：幂等检查 alias 是否存在（exit 0=存在 / 1=不存在 / 2=SDK 错误）
    p_check = sub.add_parser("check", help="幂等检查模板 alias 是否已存在")
    p_check.add_argument("--name", required=True, help="模板 alias（= --name）")
    p_check.set_defaults(func=check_alias)

    # register 子命令：REST POST /templates 注册已 docker build 镜像（CubeSandbox 唯一可用路径）
    p_reg = sub.add_parser("register", help="注册已 docker build 镜像为 CubeSandbox 模板（REST POST /templates）")
    p_reg.add_argument("--image", required=True, help="节点本地已构建的 docker image（如 agentflow-sandbox-warm:v3）")
    p_reg.add_argument("--writable-layer-size", default="1G", help="可写层大小（默认 1G，camelCase 必填）")
    p_reg.add_argument("--http-port", type=int, default=8080, help="沙箱内 HTTP server 监听端口（默认 8080）")
    p_reg.add_argument("--timeout", type=int, default=300, help="轮询模板 READY 超时秒数（默认 300）")
    p_reg.add_argument("--poll-interval", type=int, default=10, help="轮询间隔秒数（默认 10）")
    p_reg.set_defaults(func=register_template)

    # delete 子命令：REST DELETE /templates/{id} 删除模板/快照（agent 级快照回收）
    p_del = sub.add_parser("delete", help="删除平台模板/快照（REST DELETE /templates/{id}）")
    p_del.add_argument("--template-id", required=True, help="要删除的模板/快照 ID（如 snap-xxx）")
    p_del.set_defaults(func=delete_template)

    # 默认 build 子命令（无子命令时走 build）
    parser.add_argument("--name", default="agentflow-sandbox",
                        help="模板 alias/名称（默认 agentflow-sandbox）")
    parser.add_argument("--from-template", default="base",
                        help="基础模板 ID（默认 base）")
    parser.add_argument("--cpu", type=int, default=2,
                        help="CPU 核数（默认 2）")
    parser.add_argument("--memory", type=int, default=2048,
                        help="内存 MB（默认 2048）")
    parser.add_argument("--http-port", type=int, default=8080,
                        help="沙箱内 HTTP server 监听端口（默认 8080，与 config sandbox_http_port 对齐）")
    parser.set_defaults(func=build_template)

    args = parser.parse_args()
    if args.subcommand == "check":
        return check_alias(args)
    try:
        return args.func(args)
    except SystemExit:
        raise
    except Exception as e:
        _log(f"FATAL: {e}")
        import traceback
        traceback.print_exc(file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
