#!/usr/bin/env python3
"""UC1 场景级性能基线:多模态识别(商务宴请点评,带上下文变体,真实 LLM + bridge API)。

复用 scene_s2_baseline.py 的机制(bridge POST content array 含 base64 图 + SSE 流式读 [DONE]),
输入贴合领导重点 case UC1:D1 主场景(mode=outfit 点评)/ D2 输入图用归档 a3_t2i.png。
与 scene_s2 的差异:PROMPT 含"北京 + 商务宴请 + 今晚"三重上下文约束(UC1 变体)。
"""
import base64
import json
import os
import subprocess
import sys
import time
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parents[3]  # tests → bridge → frontend → aisf_agentflow
sys.path.insert(0, str(PROJECT_ROOT / "frontend" / "bridge" / "tests"))

from e2e_perf_baseline import _start_daemon  # noqa: E402
from e2e_helpers import free_port, jsonrpc_call  # noqa: E402

# ── 主场景输入(D1/D2,UC1 变体) ──
IMG = PROJECT_ROOT / ".agents/.coding_gumdam/workspace/.agent-improver/cases/case-uc1-multimodal-recognition/input/a3_t2i.png"
PROMPT = "我在北京，今晚要参加一个商务宴请，请点评我的穿搭"
LOG_DIR = PROJECT_ROOT / "logs" / "tests"
RUN_ID = f"scene-uc1-{time.strftime('%Y%m%d_%H%M%S')}"
LOG_DIR.mkdir(exist_ok=True)
RUN_LOG = LOG_DIR / RUN_ID
RUN_LOG.mkdir(parents=True, exist_ok=True)  # _start_daemon 需目录已存在(daemon.log 打开)


def main() -> None:
    os.environ.setdefault("AF_LLM_MODEL", os.environ.get("AF_LLM_MODEL", "deepseek-v4-flash"))
    daemon, port, daemon_log = _start_daemon(RUN_LOG)
    print(f"[UC1] daemon 就绪 {port} 日志 {daemon_log}")

    # 启动 bridge
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
        # 等 bridge 就绪(用 /api/health 而非 jsonrpc_call——bridge 是 HTTP 服务)
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
            print("[UC1] bridge 启动超时", file=sys.stderr)
            return
        img_b64 = base64.b64encode(IMG.read_bytes()).decode("ascii")
        t0 = time.monotonic()
        resp = requests.post(
            f"http://127.0.0.1:{bridge_port}/api/chat/outfit-assistant?stream=true",
            json={
                "user_id": f"uc1-user-{int(time.time())}",
                "client_id": f"uc1-cli-{int(time.time())}",
                "content": [
                    {"type": "text", "text": PROMPT},
                    {"type": "image_url", "image_url": {"url": f"data:image/png;base64,{img_b64}"}},
                ],
            },
            stream=True, timeout=180,
        )
        # SSE 流式读取:遍历 iter_lines 直到 [DONE] 才算端到端完成
        chunks: list[str] = []
        for line in resp.iter_lines(decode_unicode=True):
            if line is None:
                continue
            chunks.append(line)
            if line.strip() == "[DONE]":
                break
        elapsed = time.monotonic() - t0
        text = "".join(chunks)
        print(f"[UC1] 端到端: {elapsed:.1f}s | HTTP {resp.status_code} | 响应 {len(text)} 字符")

        report = {
            "ts": RUN_ID,
            "scenario": "UC1 多模态识别(商务宴请点评,带上下文变体)",
            "input": {"prompt": PROMPT, "image": str(IMG)},
            "elapsed_s": round(elapsed, 1),
            "response_chars": len(text),
            "http_status": resp.status_code,
            "log_path": str(daemon_log),
            "response_tail": text[-600:],
        }
        out = PROJECT_ROOT / ".agents/.coding_gumdam/workspace/.agent-improver/runs" / RUN_ID
        out.mkdir(parents=True, exist_ok=True)
        (out / "raw-results.json").write_text(json.dumps(report, ensure_ascii=False, indent=2))
        print(f"[UC1] 报告已存: {out / 'raw-results.json'}")
    finally:
        bridge_proc.terminate()
        if daemon.poll() is None:
            daemon.terminate()


if __name__ == "__main__":
    main()
