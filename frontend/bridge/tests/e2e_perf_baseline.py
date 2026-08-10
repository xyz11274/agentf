"""标准 E2E 性能基线脚本 — 固化真实用户问题为标准流程，测量分阶段耗时。

用途：
  将真实 E2E 用户问题固化为可重复的标准流程，建立性能基线。任何执行优化
  （如工具混合批次并行）落地前后运行本脚本，对比分阶段耗时即可量化收益。

标准流程（复用 2026-08-08 真实会话的三轮新用户对话）：
  轮 1: "帮我推荐一套穿搭"                    → coordinator 转达 SKILL 提问
  轮 2: "我在北京，朋友聚会，复古"            → SKILL 查天气 + 追问体型/性别/身高
  轮 3: "男装，匀称，没有，170"               → SKILL 出完整方案 + 图片 + 建档案

测量点（从 daemon.log 解析）：
  - 每轮总耗时（发送 → 最终响应）
  - 沙箱创建（create_latency）
  - coordinator / skill_executor 节点执行耗时（NODE_EXECUTE_SUCCESS Duration）
  - LLM 调用次数与总耗时（LLM_REQ latency）
  - 工具执行次数与总耗时（tool_exec duration_ms）——含 batch 并行后对比

运行：
  cd frontend/bridge/tests
  set -a && source ../../../examples/agent/outfit-assistant/.env && set +a
  python3 e2e_perf_baseline.py
  （真实 LLM key 来自 agent 包 .env；E2B key 由 daemon 从 configs/test/agentflowd.json 读取）
"""

from __future__ import annotations

import json
import os
import re
import signal
import socket
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from e2e_helpers import (  # noqa: E402
    DAEMON_BINARY,
    OUTFIT_AGENT,
    OUTFIT_AGENT_STR,
    PROJECT_ROOT,
    _daemon_env,
    await_agent_ready,
    free_port,
    poll_agent_response,
    wait_port,
)

# ── 标准用户问题序列（真实会话固化）──────────────────────────────
STANDARD_QUESTIONS = [
    "帮我推荐一套穿搭",
    "我在北京，朋友聚会，复古",
    "男装，匀称，没有，170",
]

# 每轮超时（真实 LLM 慢，放宽）
ROUND_TIMEOUT = 300
# 日志根目录
LOG_ROOT = PROJECT_ROOT / "logs" / "tests"


def _jsonrpc_call(host: str, port: int, method: str,
                  params: dict, req_id: int = 1, timeout: float = 360) -> dict:
    """发送 JSON-RPC 请求到 daemon，返回单行响应（长超时版）。

    e2e_helpers.jsonrpc_call 的 socket 超时只有 30s，而真实 LLM 链路的
    agent.execute 是**同步等待整个对话完成**才响应（一轮 30-90s），
    30s 必超时。这里用 360s 超时。
    """
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.settimeout(timeout)
        sock.connect((host, port))
        payload = json.dumps({
            "jsonrpc": "2.0", "method": method,
            "params": params, "id": req_id,
        })
        sock.sendall((payload + "\n").encode("utf-8"))
        chunks = []
        while True:
            ch = sock.recv(1)
            if not ch or ch == b"\n":
                break
            chunks.append(ch)
        return json.loads(b"".join(chunks).decode("utf-8"))


def _send_message(host: str, port: int, content: str, task_id: str) -> dict:
    """通过 daemon 发送用户消息（长超时 agent.execute）。"""
    return _jsonrpc_call(host, port, "agent.execute", {
        "agent_id": "outfit-assistant",
        "user_id": "perf-user",
        "client_id": task_id,
        "content": content,
        "request_id": task_id,
    }, req_id=7)


def _start_daemon(log_dir: Path) -> tuple[subprocess.Popen, int, Path]:
    """启动 outfit-assistant daemon（真实 LLM 模式，从宿主机环境读 key）。"""
    host_api_key = os.environ.get("AF_LLM_API_KEY", "")
    host_base_url = os.environ.get("AF_LLM_BASE_URL", "")
    if not (host_api_key and host_base_url):
        raise SystemExit(
            "缺少真实 LLM 配置。请先: set -a && source "
            "examples/agent/outfit-assistant/.env && set +a"
        )

    daemon_port = free_port()
    daemon_log = log_dir / "daemon.log"
    env = _daemon_env()
    env["PATH"] = (
        f"{OUTFIT_AGENT / '.venv' / 'bin'}"
        f"{os.pathsep}/usr/bin:/bin:/usr/sbin:/sbin"
    )
    env.setdefault("AF_LLM_BASE_URL", host_base_url)
    env.setdefault("AF_LLM_MODEL", os.environ.get("AF_LLM_MODEL", "deepseek-v4-flash"))
    env.setdefault("AF_LLM_API_KEY", host_api_key)
    env.setdefault("AF_SKILL_MAX_COMPLETION_TOKENS", "512")
    env["NO_PROXY"] = "127.0.0.1,localhost"
    env["no_proxy"] = "127.0.0.1,localhost"
    env.setdefault(
        "PYTHONPATH",
        str(PROJECT_ROOT / "packages" / "agentflow-runtime" / "src"),
    )
    env["AF_LOG_FILE"] = str(daemon_log)

    daemon = subprocess.Popen(
        [str(DAEMON_BINARY), "--listen", f"127.0.0.1:{daemon_port}",
         "--package", OUTFIT_AGENT_STR],
        cwd=PROJECT_ROOT, env=env,
        stdout=daemon_log.open("wb"), stderr=subprocess.STDOUT,
    )
    wait_port(daemon_port, daemon)
    await_agent_ready(daemon_port, "outfit-assistant", daemon)
    return daemon, daemon_port, daemon_log


def _parse_daemon_log(log: Path) -> dict:
    """从 daemon.log 解析分阶段测量点（节点耗时/LLM/工具）。"""
    text = log.read_text(errors="replace")

    # 节点执行耗时（最后一次各节点的 Duration）
    node_durations: dict[str, list[int]] = {}
    for m in re.finditer(
        r"NODE_EXECUTE_SUCCESS\].*?NodeId=(\w+).*?Duration=(\d+)ms",
        text,
    ):
        node_durations.setdefault(m.group(1), []).append(int(m.group(2)))

    # 沙箱创建耗时（最后一次 create_latency）
    sandbox_latencies = [
        int(x) for x in re.findall(r"create_latency=(\d+)ms", text)
    ]

    # LLM 调用
    llm_latencies = [
        int(x) for x in re.findall(r"latency=(\d+)ms", text)
    ]

    # 工具执行
    tool_execs: dict[str, list[int]] = {}
    for m in re.finditer(r"tool_exec name=(\w+) duration_ms=([\d.]+)", text):
        tool_execs.setdefault(m.group(1), []).append(float(m.group(2)))

    def _last(seq):
        return seq[-1] if seq else None

    return {
        "sandbox_create_ms": _last(sandbox_latencies),
        "coordinator_decide_ms": _last(node_durations.get("coordinator_decide", [])),
        "skill_executor_ms": _last(node_durations.get("skill_executor", [])),
        "llm_calls": len(llm_latencies),
        "llm_total_ms": sum(llm_latencies),
        "llm_avg_ms": round(sum(llm_latencies) / len(llm_latencies)) if llm_latencies else None,
        "tools": {
            name: {"count": len(v), "total_ms": round(sum(v), 1), "avg_ms": round(sum(v) / len(v), 1)}
            for name, v in tool_execs.items()
        },
        "tools_total_ms": round(sum(sum(v) for v in tool_execs.values()), 1),
    }


def main() -> None:
    ts = time.strftime("%Y%m%d_%H%M%S")
    log_dir = LOG_ROOT / f"{ts}_e2e_perf_baseline"
    log_dir.mkdir(parents=True, exist_ok=True)

    print(f"=== E2E 性能基线（真实 LLM + e2b，{len(STANDARD_QUESTIONS)} 轮）===")
    daemon, port, daemon_log = _start_daemon(log_dir)
    print(f"daemon 就绪: 127.0.0.1:{port}  日志: {daemon_log}")

    rounds: list[dict] = []
    try:
        for i, question in enumerate(STANDARD_QUESTIONS, 1):
            task_id = f"perf-baseline-{i}-{int(time.time() * 1000)}"
            t0 = time.monotonic()
            _send_message("127.0.0.1", port, question, task_id)
            response = poll_agent_response(
                "127.0.0.1", port, task_id, timeout=ROUND_TIMEOUT
            )
            elapsed = time.monotonic() - t0
            rounds.append({
                "round": i,
                "question": question,
                "elapsed_s": round(elapsed, 1),
                "response_chars": len(response),
            })
            print(f"  轮{i}: {elapsed:.1f}s（{len(response)} 字符）")

    finally:
        if daemon.poll() is None:
            daemon.send_signal(signal.SIGTERM)
            try:
                daemon.wait(timeout=12)
            except subprocess.TimeoutExpired:
                daemon.kill()
                daemon.wait(timeout=3)

    # 解析分阶段测量点
    stages = _parse_daemon_log(daemon_log)
    report = {
        "ts": ts,
        "rounds": rounds,
        "total_elapsed_s": round(sum(r["elapsed_s"] for r in rounds), 1),
        "stages": stages,
        "log_path": str(daemon_log),
    }

    report_path = log_dir / "baseline_report.json"
    report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2))
    print("\n=== 基线报告 ===")
    print(json.dumps(report, ensure_ascii=False, indent=2))
    print(f"\n报告已保存: {report_path}")


if __name__ == "__main__":
    main()
