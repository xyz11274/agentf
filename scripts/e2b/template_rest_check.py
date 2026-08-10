#!/usr/bin/env python3
"""CubeSandbox 模板 REST 接口可用性验证脚本（SC-v2 验证前置）。

背景：当前部署（218.245.96.148:30030）是 CubeSandbox（e2b SDK 兼容的自托管沙箱），
SDK 的 `Template.alias_exists`/`Template.build` 端点在该平台不实现（404）。
v2 预热方案的"模板幂等检查"若适配 CubeSandbox，需改用其 REST API。本脚本验证
REST 接口的可用性与字段结构，作为适配决策的证据。

验证项：
  1. GET /templates 列表（200 + JSON 数组 + templateID 字段结构）
  2. GET /templates/{templateID} 单查（200，已存在模板）
  3. GET /templates/{agentflow-sandbox-{agent_id}}（404 = 未预建，幂等检查"未命中"分支）
  4. POST /templates 字段探测（不带/带部分字段 → 422/500 错误提示，证明创建端点存在）

用法:
  python3 scripts/e2b/template_rest_check.py                          # 默认 API + outfit-assistant
  python3 scripts/e2b/template_rest_check.py --api-url http://x:30030
  python3 scripts/e2b/template_rest_check.py --agent-id my-agent --target-template tpl-xxx

环境变量:
  E2B_API_URL    API 地址（默认 http://218.245.96.148:30030）
  E2B_API_KEY    API 密钥（可选，部分端点可能要求鉴权）
"""

import argparse
import json
import os
import sys
import urllib.error
import urllib.request
from datetime import datetime
from pathlib import Path

# ── 默认配置（与 scripts/e2b/sandbox_list.py 同源）────────────────────
_DEFAULT_API_URL = "http://218.245.96.148:30030"
_DEFAULT_API_KEY = "e2b_000000000"
_CA_CERT_PATH = str(Path(__file__).resolve().parent.parent.parent / "resource" / "certs" / "e2b_CA.pem")
_LOG_DIR = Path(__file__).resolve().parent.parent.parent / "logs" / "tests"


def _log(msg: str) -> None:
    print(f"[template_rest_check] {msg}", file=sys.stderr, flush=True)


def _setup_env(api_url: str, api_key: str) -> None:
    """设置 API 地址与密钥环境（不输出密钥明文）。"""
    if api_url:
        os.environ["E2B_API_URL"] = api_url
    else:
        os.environ.setdefault("E2B_API_URL", _DEFAULT_API_URL)
    if api_key:
        os.environ["E2B_API_KEY"] = api_key
    else:
        os.environ.setdefault("E2B_API_KEY", _DEFAULT_API_KEY)
    # CA 证书：https 端点（path-based 路由 443）需要；http 管理 API 可选
    if not os.environ.get("SSL_CERT_FILE") and Path(_CA_CERT_PATH).is_file():
        os.environ["SSL_CERT_FILE"] = _CA_CERT_PATH
    _log(f"API URL: {os.environ.get('E2B_API_URL')}")


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


def cmd_check(args: argparse.Namespace) -> int:
    _setup_env(args.api_url, args.api_key)
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
        # 检查目标模板是否在列表中（存在性）
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
        f"未预建 alias 返回 404（幂等'未命中'可据此判定）",
        ok,
        f"HTTP {code}" + (f", body={str(data)[:120]}" if code != 404 else ", 404 非 JSON 属正常"),
    )
    results.append(ok)

    # ── 4. POST /templates 字段探测（创建端点存在性）───────────────
    print("\n[4] POST /templates 字段探测（创建端点存在性，不实际创建）")
    code, data = _check("POST", f"{base}/templates", {})
    # 期望：422（缺字段）或 500（CubeMaster 参数校验）—— 都证明端点存在且非 404
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


def main() -> int:
    parser = argparse.ArgumentParser(description="CubeSandbox 模板 REST 接口可用性验证")
    parser.add_argument("--api-url", default=os.environ.get("E2B_API_URL", ""),
                        help="API 地址（默认环境变量 E2B_API_URL 或内置默认）")
    parser.add_argument("--api-key", default=os.environ.get("E2B_API_KEY", ""),
                        help="API 密钥（默认环境变量 E2B_API_KEY）")
    parser.add_argument("--agent-id", default="outfit-assistant",
                        help="agent_id，用于构造幂等 alias（agentflow-sandbox-{agent_id}）")
    parser.add_argument("--target-template", default="tpl-cb6b9dcbf5784a1487a75971",
                        help="目标已有模板 templateID")
    args = parser.parse_args()
    try:
        return cmd_check(args)
    except Exception as e:
        _log(f"FATAL: {e}")
        import traceback
        traceback.print_exc(file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
