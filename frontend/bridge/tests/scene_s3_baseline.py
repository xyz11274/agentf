#!/usr/bin/env python3
"""S3/C1 场景级性能基线:文本→图(推荐 + 效果图生成,真实 LLM + bridge API)。

输入贴合主场景(case-aisf-perf-003 instruction):"帮我推荐一套通勤穿搭,并在最后生成一张效果图"
纯文本请求,触发 skill 完整流程(查档案/天气 + 推荐 + generate.py 图生成)。
"""
import base64
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

# ── 主场景输入(D1/D2, case-aisf-perf-003) ──
PROMPT = "帮我推荐一套通勤穿搭,并在最后生成一张效果图"
RUN_ID = f"scene-s3-{time.strftime('%Y%m%d_%H%M%S')}"
RUN_LOG = PROJECT_ROOT / "logs" / "tests" / RUN_ID
RUN_LOG.mkdir(parents=True, exist_ok=True)


def main() -> None:
    os.environ.setdefault("AF_LLM_MODEL", os.environ.get("AF_LLM_MODEL", "deepseek-v4-flash"))
    daemon, port, daemon_log = _start_daemon(RUN_LOG)
    print(f"[S3] daemon 就绪 {port}")

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
            print("[S3] bridge 启动超时", file=sys.stderr)
            return

        t0 = time.monotonic()
        resp = requests.post(
            f"http://127.0.0.1:{bridge_port}/api/chat/outfit-assistant?stream=true",
            json={
                "user_id": f"s3-user-{int(time.time())}",
                "client_id": f"s3-cli-{int(time.time())}",
                "content": [{"type": "text", "text": PROMPT}],
            },
            stream=True, timeout=240,
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
        print(f"[S3] 端到端: {elapsed:.1f}s | HTTP {resp.status_code} | 响应 {len(text)} 字符")

        report = {
            "ts": RUN_ID,
            "scenario": "S3 文本→图(t2i)",
            "input": {"prompt": PROMPT},
            "elapsed_s": round(elapsed, 1),
            "response_chars": len(text),
            "http_status": resp.status_code,
            "log_path": str(daemon_log),
        }
        out = PROJECT_ROOT / ".agents/.coding_gumdam/workspace/.agent-improver/runs" / RUN_ID
        out.mkdir(parents=True, exist_ok=True)
        (out / "raw-results.json").write_text(json.dumps(report, ensure_ascii=False, indent=2))
        print(f"[S3] 报告已存: {out / 'raw-results.json'}")
    finally:
        bridge_proc.terminate()
        if daemon.poll() is None:
            daemon.terminate()


if __name__ == "__main__":
    main()
