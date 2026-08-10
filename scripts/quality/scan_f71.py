#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
scan_f71.py — F7.1 外部调用落盘合规检测

铁律 F7.1（constitution.md）:与外部服务（LLM API、HTTP/UDS/MCP 等）交互时,
原始请求体与原始响应体必须以完整内容落盘（日志/dump）。任何截断或缺失都会让
现场证据失效。本脚本将这条"规则文本"转化为可执行检测,形成
"检测→决策→行动→度量"闭环的第一环。

检测规则:
  对指定目录下所有源文件,找出外部 HTTP 发送调用点:
    - .Send(            # CoreMind H2 客户端
    - cli.Post( / .Post( # httplib / 通用
  检查每个调用点"上下文窗口"（调用行前后 CONTEXT_LINES 行）内是否存在:
    - dump 记录: dump_callback_request / dump_callback_result / dump_request /
      dump_sse_chunk / dump_callback
    - 或完整响应日志: resp_body= / resp_body_len 且带完整打印 / logging
    - 或显式豁免标记: f71-exempt
  不满足 = 不合规调用点,输出清单 + 退出码 1。

用法:
  python3 scripts/quality/scan_f71.py [--dir sidecar] [--quick]
  --quick  只扫描 git 改动的文件（prepush 场景）

退出码: 0 = 全部合规;1 = 存在不合规调用点。
"""

import argparse
import os
import re
import subprocess
import sys

PROJECT_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))

# 外部发送调用点匹配（C++/Python 通用;排除纯接收端 svr.Post / svr.Get 等）
SEND_PATTERNS = [
    re.compile(r"(?:->|\.)\s*Send\s*\("),          # h2->Send( / client.Send(
    re.compile(r"\bcli\s*\.\s*Post\s*\("),          # cli.Post( / cb_cli.Post( / sync_cli.Post(
    re.compile(r"\b(?:http2Client|client|cb_cli|ts_cli|sync_cli)\s*\.\s*Post\s*\("),
    re.compile(r"\.\s*Post\s*\("),
    re.compile(r"\.\s*Get\s*\("),
]

# 接收端服务注册（svr.Post / svr.Get / register 等,不是"发送给外部"）
RECEIVE_WHITELIST = re.compile(
    r"\b(?:svr|server|app)\s*\.\s*(?:Post|Get|Put|Delete)\s*\(|"
    r"Register\s*\(|URLPatterns\s*\("
)

# 合规证据:请求/响应完整记录标记
COMPLIANCE_MARKERS = [
    re.compile(r"dump_callback_request"),
    re.compile(r"dump_callback_result"),
    re.compile(r"dump_request\s*\("),
    re.compile(r"dump_sse_chunk"),
    re.compile(r"dump_callback\s*\("),
    re.compile(r"resp_body="),           # 原样打印响应体
    re.compile(r"resp_body_len"),        # 响应长度（弱证据,仅提示）
    re.compile(r"logging\.(info|debug|error|warning)"),
    re.compile(r"f71-exempt"),           # 显式豁免
]

STRONG_MARKERS = [
    re.compile(r"dump_callback_request"),
    re.compile(r"dump_callback_result"),
    re.compile(r"dump_request\s*\("),
    re.compile(r"dump_sse_chunk"),
    re.compile(r"dump_callback\s*\("),
    re.compile(r"req_body="),            # 原样打印请求体
    re.compile(r"resp_body="),           # 原样打印响应体
    re.compile(r"f71-exempt"),
]

# 豁免模式:测试文件 / 静态 stub 响应 / 纯本地模拟
EXEMPT_PATTERNS = [
    re.compile(r"test_"),
    re.compile(r"_test\.|\.test\."),
    re.compile(r"stub"),
    re.compile(r"mock"),
]

CONTEXT_LINES = 30


def is_exempt_file(path):
    """按文件名判断豁免（测试/stub/mock）。"""
    base = os.path.basename(path)
    return any(p.search(base) for p in EXEMPT_PATTERNS)


def is_receive_call(line):
    """判断是否为接收端注册（svr.Post 等,不是外部发送）。"""
    return bool(RECEIVE_WHITELIST.search(line))


def is_send_call(line):
    return any(p.search(line) for p in SEND_PATTERNS)


def find_send_points(lines):
    """返回 [(line_no_1based, 调用行), ...] 过滤掉接收端注册。"""
    points = []
    for i, line in enumerate(lines):
        if is_receive_call(line):
            continue
        if is_send_call(line):
            points.append((i + 1, line.strip()))
    return points


def check_context(lines, idx, filepath):
    """检查调用点上下文窗口内是否有合规证据。

    idx: 0-based 调用行下标。
    返回 (strong, weak): strong=True 有强证据;weak=True 有弱证据（len 类）。
    """
    start = max(0, idx - CONTEXT_LINES)
    end = min(len(lines), idx + CONTEXT_LINES + 1)
    window = "\n".join(lines[start:end])

    has_strong = any(m.search(window) for m in STRONG_MARKERS)
    has_weak = any(m.search(window) for m in COMPLIANCE_MARKERS)
    return has_strong, has_weak


def scan_file(path):
    """扫描单个文件,返回 (合规点, 不合规点, 弱证据点, 豁免)。"""
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        lines = f.readlines()

    if is_exempt_file(path):
        return [], [], [], [path]

    send_points = find_send_points(lines)
    compliant, noncompliant, weak = [], [], []
    for line_no, call_line in send_points:
        idx = line_no - 1
        strong, weak_ok = check_context(lines, idx, path)
        rel = os.path.relpath(path, PROJECT_ROOT)
        if strong:
            compliant.append((rel, line_no, call_line))
        elif weak_ok:
            weak.append((rel, line_no, call_line))
        else:
            noncompliant.append((rel, line_no, call_line))
    return compliant, noncompliant, weak, []


def git_changed_files():
    try:
        out = subprocess.check_output(
            ["git", "diff", "HEAD", "--name-only", "--diff-filter=ACM"],
            cwd=PROJECT_ROOT, stderr=subprocess.DEVNULL, text=True,
        )
    except subprocess.CalledProcessError:
        return []
    return [os.path.join(PROJECT_ROOT, f) for f in out.splitlines() if f]


def main():
    parser = argparse.ArgumentParser(description="F7.1 外部调用落盘合规检测")
    parser.add_argument("--dir", default="sidecar",
                        help="扫描目录（相对项目根）,默认 sidecar")
    parser.add_argument("--quick", action="store_true",
                        help="只扫描 git 改动的文件")
    args = parser.parse_args()

    if args.quick:
        files = git_changed_files()
        # 只保留目标目录下的文件
        target = os.path.abspath(os.path.join(PROJECT_ROOT, args.dir))
        files = [f for f in files if f.startswith(target)]
    else:
        target = os.path.abspath(os.path.join(PROJECT_ROOT, args.dir))
        if not os.path.isdir(target):
            print(f"[scan_f71] 目录不存在: {target}")
            sys.exit(2)
        files = []
        for root, dirs, fnames in os.walk(target):
            dirs[:] = [d for d in dirs if d not in (".git", "build", "build-dev",
                                                    "build-test", "build-production",
                                                    "__pycache__", "node_modules")]
            for fn in fnames:
                if fn.endswith((".cpp", ".h", ".hpp", ".py")):
                    files.append(os.path.join(root, fn))

    if not files:
        print("[scan_f71] 无待扫描文件")
        sys.exit(0)

    all_compliant, all_noncompliant, all_weak, all_exempt = [], [], [], []
    for path in sorted(files):
        c, nc, w, ex = scan_file(path)
        all_compliant.extend(c)
        all_noncompliant.extend(nc)
        all_weak.extend(w)
        all_exempt.extend(ex)

    print("═" * 64)
    print("  F7.1 外部调用落盘合规检测")
    print("═" * 64)

    if all_exempt:
        print(f"\n[豁免] {len(all_exempt)} 个文件（测试/stub/mock）:")
        for f in all_exempt:
            print(f"  ⏭️  {os.path.relpath(f, PROJECT_ROOT)}")

    if all_compliant:
        print(f"\n[合规] {len(all_compliant)} 个调用点有完整请求/响应记录:")
        for rel, ln, call in all_compliant:
            print(f"  ✅ {rel}:{ln}  {call}")

    if all_weak:
        print(f"\n[弱证据] {len(all_weak)} 个调用点仅有长度类记录（建议补完整响应体）:")
        for rel, ln, call in all_weak:
            print(f"  ⚠️  {rel}:{ln}  {call}")

    if all_noncompliant:
        print(f"\n[不合规] {len(all_noncompliant)} 个调用点无请求/响应落盘记录(F7.1 违反):")
        for rel, ln, call in all_noncompliant:
            print(f"  ❌ {rel}:{ln}  {call}")
        print("\n  修复指引: 在调用点前后补 dump_callback_request/dump_callback_result")
        print("  （参考 sidecar_router.cpp 的 SidecarDumper 用法）或显式加 // f71-exempt")
        print("  并在理由注释中说明为何豁免。")
    else:
        print("\n[结果] 全部外部调用点已具备 F7.1 落盘记录 ✅")

    total = len(all_compliant) + len(all_weak) + len(all_noncompliant)
    rate = (len(all_compliant) + len(all_weak)) / total * 100 if total else 100.0
    print(f"\n[度量] 外部调用点共 {total} 个,合规率(含弱证据) {rate:.1f}%")

    sys.exit(1 if all_noncompliant else 0)


if __name__ == "__main__":
    main()
