#!/usr/bin/env python3
"""C2/C3 场景级性能基线:图生图(i2i)/图生视频(i2v),真实 LLM + bridge API。

用法: python3 scene_text_generic.py --prompt "..." --run-id scene-c2-xxx [--timeout 300]
输入贴合主场景(case-aisf-perf-004/005 instruction),纯文本请求触发 skill 完整流程。
"""
import argparse
import json
import os
import subprocess
import sys
import time
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(PROJECT_ROOT / "frontend" / "bridge" / "tests"))

from e2e_perf_baseline import _start_daemon  # noqa: E402
from e2e_helpers import free_port  # noqa: E402


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--prompt", required=True)
    ap.add_argument("--run-id", required=True)
    ap.add_argument("--timeout", type=int, default=300)
    args = ap.parse_args()

    os.environ.setdefault("AF_LLM_MODEL", os.environ.get("AF_LLM_MODEL", "deepseek-v4-flash"))
    RUN_LOG = PROJECT_ROOT / "logs" / "tests" / args.run_id
    RUN_LOG.mkdir(parents=True, exist_ok=True)
    daemon, port, daemon_log = _start_daemon(RUN_LOG)
    print(f"[{args.run_id}] daemon 就绪 {port}")

    bridge_port = free_port()
    bridge_dir = PROJECT_ROOT / "frontend" / "bridge"
    env = dict(os.environ)
    env["AF_BR_ENV"] = "test"
    env["AF_BR_DAEMON_HOST"] = "127.0.0.1"
    env["AF_BR_DAEMON_PORT"] = str(port)
    env["BRIDGE_HTTP_PORT"] = str(bridge_port)
    env["NO_PROXY"] = "127.0.0.1,localhost"
    env["no_proxy"] = "127.0.0.1,localhost"
    bridge_proc = subprocess.Popen(
        ["uv", "run", "--directory", str(bridge_dir), "python", "-m", "uvicorn",
         "main:bridge_app", "--host", "127.0.0.1", "--port", str(bridge_port),
         "--log-level", "warning"],
        cwd=str(bridge_dir), env=env, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT,
    )
    try:
        import requests
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            try:
                if requests.get(f"http://127.0.0.1:{bridge_port}/api/health", timeout=1).status_code == 200:
                    break
            except Exception:
                pass
            time.sleep(0.3)
        else:
            print(f"[{args.run_id}] bridge 启动超时", file=sys.stderr)
            return

        t0 = time.monotonic()
        uid = f"{args.run_id}-user-{int(time.time())}"
        cid = f"{args.run_id}-cli-{int(time.time())}"
        resp = requests.post(
            f"http://127.0.0.1:{bridge_port}/api/chat/outfit-assistant?stream=true",
            json={
                "user_id": uid,
                "client_id": cid,
                "content": [{"type": "text", "text": args.prompt}],
            },
            stream=True, timeout=args.timeout,
        )
        chunks: list[str] = []
        for line in resp.iter_lines(decode_unicode=True):
            if line is None:
                continue
            chunks.append(line)
            if line.strip() == "[DONE]":
                break
        elapsed = time.monotonic() - t0
        text = "".join(chunks)
        print(f"[{args.run_id}] 端到端: {elapsed:.1f}s | HTTP {resp.status_code} | 响应 {len(text)} 字符")

        # 对话完成【立即】查 llm-traces(会话存续期,DataBus 未清理——事后查会查不到,
        # 见 2026-08-11 失败经验:前端在 turn 完成后立即查,测试需同样及时)
        traces_summary: dict = {}
        try:
            tr = requests.get(
                f"http://127.0.0.1:{bridge_port}/api/agents/outfit-assistant/llm-traces",
                params={"user_id": uid, "client_id": cid},
                timeout=10,
            )
            traces = (tr.json().get("data", {}).get("traces", [])) if tr.status_code == 200 else []
            if traces:
                n = len(traces)
                tokens = [t.get("tokens", {}) for t in traces]
                prompt = sum((x.get("prompt") or 0) for x in tokens)
                completion = sum((x.get("completion") or 0) for x in tokens)
                traces_summary = {
                    "trace_count": n,
                    "tokens": {"prompt": prompt, "completion": completion, "total": prompt + completion},
                    "call_ids": [t.get("call_id") for t in traces],
                }
                print(f"[{args.run_id}] llm-traces: {n} 条 | prompt={prompt} completion={completion}")
            else:
                print(f"[{args.run_id}] llm-traces 空(可能已清理): {tr.status_code}")
        except Exception as exc:
            print(f"[{args.run_id}] llm-traces 查询失败: {exc}", file=sys.stderr)

        report = {
            "ts": args.run_id,
            "scenario": args.run_id,
            "input": {"prompt": args.prompt},
            "elapsed_s": round(elapsed, 1),
            "response_chars": len(text),
            "http_status": resp.status_code,
            "llm_traces": traces_summary,
            "log_path": str(daemon_log),
        }
        out = PROJECT_ROOT / ".agents/.coding_gumdam/workspace/.agent-improver/runs" / args.run_id
        out.mkdir(parents=True, exist_ok=True)
        (out / "raw-results.json").write_text(json.dumps(report, ensure_ascii=False, indent=2))
        print(f"[{args.run_id}] 报告已存: {out / 'raw-results.json'}")
    finally:
        bridge_proc.terminate()
        if daemon.poll() is None:
            daemon.terminate()


if __name__ == "__main__":
    main()
