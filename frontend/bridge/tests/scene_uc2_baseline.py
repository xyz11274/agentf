#!/usr/bin/env python3
"""UC2 场景级性能基线:UC1→UC2 多轮续接文生图(真实 LLM + bridge API)。

复用 scene_uc1_baseline.py 的机制(bridge POST content array 含 base64 图 + SSE 流式读 [DONE]),
两轮连续对话用**同一 client_id**(SessionRouter 按 user_id:client_id 复用 session/dialogue,
否则每轮被当独立新请求走完整流程——CHANGE-20260811-040)。
轮 1 = UC1 点评(带图);轮 2 = UC2 续接(推荐更好穿搭 + 上身效果图,纯文本)。
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

# ── 主场景输入(D1/D2,UC1→UC2 多轮续接) ──
IMG = PROJECT_ROOT / ".agents/.coding_gumdam/workspace/.agent-improver/cases/case-uc2-text-to-image/input/a3_t2i.png"
PROMPT_UC1 = "我在北京，今晚要参加一个商务宴请，请点评我的穿搭"
PROMPT_UC2 = "帮我推荐更好的穿搭建议, 并给我上身效果图"
LOG_DIR = PROJECT_ROOT / "logs" / "tests"
RUN_ID = f"scene-uc2-{time.strftime('%Y%m%d_%H%M%S')}"
LOG_DIR.mkdir(exist_ok=True)
RUN_LOG = LOG_DIR / RUN_ID
RUN_LOG.mkdir(parents=True, exist_ok=True)  # _start_daemon 需目录已存在(daemon.log 打开)


def _sse_chat(bridge_port: int, user_id: str, client_id: str,
              content: list, timeout: float = 240) -> tuple[float, int, str]:
    """POST /api/chat/outfit-assistant?stream=true,SSE 读到 [DONE],返回 (elapsed, http, text)。"""
    import requests
    img_payload = []
    for item in content:
        if item.get("type") == "image_url":
            img_b64 = base64.b64encode(IMG.read_bytes()).decode("ascii")
            img_payload.append({"type": "image_url",
                                "image_url": {"url": f"data:image/png;base64,{img_b64}"}})
        else:
            img_payload.append(item)
    t0 = time.monotonic()
    resp = requests.post(
        f"http://127.0.0.1:{bridge_port}/api/chat/outfit-assistant?stream=true",
        json={"user_id": user_id, "client_id": client_id, "content": img_payload},
        stream=True, timeout=timeout,
    )
    chunks: list[str] = []
    for line in resp.iter_lines(decode_unicode=True):
        if line is None:
            continue
        chunks.append(line)
        if line.strip() == "[DONE]":
            break
    elapsed = time.monotonic() - t0
    return elapsed, resp.status_code, "".join(chunks)


def main() -> None:
    os.environ.setdefault("AF_LLM_MODEL", os.environ.get("AF_LLM_MODEL", "deepseek-v4-flash"))
    daemon, port, daemon_log = _start_daemon(RUN_LOG)
    print(f"[UC2] daemon 就绪 {port} 日志 {daemon_log}")

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
            print("[UC2] bridge 启动超时", file=sys.stderr)
            return

        uid = f"uc2-user-{int(time.time())}"
        cid = f"uc2-cli-{int(time.time())}"  # 两轮同一 client_id → 复用 session

        # 轮 1 = UC1 点评(带图)
        e1, s1, t1 = _sse_chat(bridge_port, uid, cid,
                               [{"type": "text", "text": PROMPT_UC1},
                                {"type": "image_url", "image_url": {"url": ""}}])
        print(f"[UC2] 轮1(UC1 点评): {e1:.1f}s | HTTP {s1} | {len(t1)} 字符")

        # 轮 2 = UC2 续接(推荐 + 效果图,纯文本)
        e2, s2, t2 = _sse_chat(bridge_port, uid, cid,
                               [{"type": "text", "text": PROMPT_UC2}])
        print(f"[UC2] 轮2(UC2 文生图): {e2:.1f}s | HTTP {s2} | {len(t2)} 字符")
        print(f"[UC2] 合计: {e1 + e2:.1f}s")

        report = {
            "ts": RUN_ID,
            "scenario": "UC2 文生图(UC1→UC2 多轮续接变体)",
            "input": {"prompt_uc1": PROMPT_UC1, "prompt_uc2": PROMPT_UC2, "image": str(IMG)},
            "rounds": [
                {"round": 1, "scenario": "UC1 点评", "elapsed_s": round(e1, 1),
                 "response_chars": len(t1), "http_status": s1},
                {"round": 2, "scenario": "UC2 文生图", "elapsed_s": round(e2, 1),
                 "response_chars": len(t2), "http_status": s2},
            ],
            "total_elapsed_s": round(e1 + e2, 1),
            "log_path": str(daemon_log),
            "response_uc2_tail": t2[-800:],
        }
        out = PROJECT_ROOT / ".agents/.coding_gumdam/workspace/.agent-improver/runs" / RUN_ID
        out.mkdir(parents=True, exist_ok=True)
        (out / "raw-results.json").write_text(json.dumps(report, ensure_ascii=False, indent=2))
        print(f"[UC2] 报告已存: {out / 'raw-results.json'}")
    finally:
        bridge_proc.terminate()
        if daemon.poll() is None:
            daemon.terminate()


if __name__ == "__main__":
    main()
