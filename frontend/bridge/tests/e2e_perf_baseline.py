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


def _send_message(host: str, port: int, content: str, task_id: str,
                  client_id: str | None = None) -> dict:
    """通过 daemon 发送用户消息（长超时 agent.execute）。

    client_id 决定对话路由（SessionRouter 按 user_id:client_id 复用同一
    session/dialogue）。同一对话的澄清回复必须传相同 client_id，否则每轮
    都被当作独立新请求走完整流程（测试案例设计修正，CHANGE-20260811-040）。
    """
    return _jsonrpc_call(host, port, "agent.execute", {
        "agent_id": "outfit-assistant",
        "user_id": "perf-user",
        "client_id": client_id or task_id,
        "content": content,
        "request_id": task_id,
    }, req_id=7)


def _ensure_snapshot_fresh(agent_pkg: Path, registry_path: Path,
                           build_pkg: Path | None = None) -> None:
    """评测前确保 e2b snapshot 与 manifest hash 一致（避免 daemon 启动 rebuild 超 15s）。

    背景（2026-08-12 实测）：修改 harness 文件（SKILL.md/工具）后 manifest hash 变化，
    daemon 启动时检测 stale → 触发 rebuild（140-260s）> wait_port 15s → 评测启动失败。
    本函数在启动 daemon 前检查 registry hash vs manifest hash：
      - 一致 → 跳过（无需预构建）
      - 不一致 → 调 template_admin.py snapshot --manifest 预构建 + 更新 registry

    三态模型（agent-package-three-state.md）：② 固化态 = resource/agent-packages/{name}（纯净
    静态资产权威源），③ workspace = examples/agent/{name}（② 的拷贝 + 运行时数据）。daemon
    加载 ③（--package examples/...），故 hash 检测用 agent_pkg（③，与 daemon 口径一致）；
    快照构建 asset 源用 build_pkg（② 固化态，默认 resource/agent-packages/{name}）。
    """
    hash_script = PROJECT_ROOT / "scripts" / "e2b" / "template_admin.py"
    manifest = agent_pkg / "manifest.jsonc"
    if build_pkg is None:
        build_pkg = PROJECT_ROOT / "resource" / "agent-packages" / agent_pkg.name
    build_manifest = build_pkg / "manifest.jsonc"

    def _manifest_hash() -> str:
        r = subprocess.run(
            [sys.executable, str(hash_script), "snapshot", "--hash-manifest", str(manifest)],
            capture_output=True, text=True, cwd=PROJECT_ROOT, timeout=120,
        )
        lines = [ln.strip() for ln in r.stdout.splitlines() if ln.strip()]
        return lines[-1] if lines else ""

    def _registry_hash() -> str:
        try:
            with open(registry_path, encoding="utf-8") as f:
                doc = json.load(f)
            entries = doc.get("entries") or []
            return entries[0].get("hash", "") if entries else ""
        except (OSError, json.JSONDecodeError, IndexError):
            return ""

    mhash = _manifest_hash()
    rhash = _registry_hash()
    if mhash and mhash == rhash:
        print(f"[hash-check] snapshot 最新（hash={mhash}），跳过预构建")
        return

    print(f"[hash-check] snapshot stale（registry={rhash} vs manifest={mhash}），预构建中…")
    r = subprocess.run(
        [sys.executable, str(hash_script), "snapshot", "--manifest", str(build_manifest)],
        capture_output=True, text=True, cwd=PROJECT_ROOT, timeout=600,
    )
    lines = [ln.strip() for ln in r.stdout.splitlines() if ln.strip()]
    snap_id = lines[-1] if lines else ""
    if not snap_id.startswith("snap-"):
        raise SystemExit(f"[hash-check] 预构建失败，无法解析 snapshotID。stdout 尾部:\n{r.stdout[-500:]}\nstderr:\n{r.stderr[-500:]}")
    # 更新 registry（hash + snapshot 同步指向新快照）
    try:
        with open(registry_path, encoding="utf-8") as f:
            doc = json.load(f)
    except (OSError, json.JSONDecodeError):
        doc = {"entries": []}
    if not doc.get("entries"):
        doc["entries"] = [{}]
    doc["entries"][0] = {
        "agent_id": "outfit-assistant",
        "snapshot": snap_id,
        "version": "3.0.0",
        "hash": mhash,
    }
    registry_path.parent.mkdir(parents=True, exist_ok=True)
    with open(registry_path, "w", encoding="utf-8") as f:
        json.dump(doc, f, ensure_ascii=False)
    print(f"[hash-check] 预构建完成 snapshot={snap_id} hash={mhash}，registry 已更新")


def _start_daemon(log_dir: Path) -> tuple[subprocess.Popen, int, Path]:
    """启动 outfit-assistant daemon（真实 LLM 模式，自动从 agent 包 .env 读 key）。"""
    # 自动 source agent 包 .env：调用方无需手动 `set -a && source .env`。
    # 缺 AF_LLM_API_KEY/AF_LLM_BASE_URL 时从 outfit-assistant 包 .env 加载到 os.environ。
    agent_env = OUTFIT_AGENT / ".env"
    if agent_env.is_file():
        try:
            from dotenv import load_dotenv
            load_dotenv(agent_env, override=False)
        except ImportError:
            # dotenv 不可用时兜底 stdlib 零依赖解析（KEY=VALUE 行，跳过 # 注释）
            for line in agent_env.read_text().splitlines():
                line = line.strip()
                if not line or line.startswith("#") or "=" not in line:
                    continue
                k, v = line.split("=", 1)
                k, v = k.strip(), v.strip().strip("'\"")
                os.environ.setdefault(k, v)

    host_api_key = os.environ.get("AF_LLM_API_KEY", "")
    host_base_url = os.environ.get("AF_LLM_BASE_URL", "")
    if not (host_api_key and host_base_url):
        raise SystemExit(
            "缺少真实 LLM 配置：未在环境变量或 agent 包 .env 找到 "
            "AF_LLM_API_KEY/AF_LLM_BASE_URL。请检查 "
            f"{agent_env} 是否存在且含 LLM key。"
        )

    # 评测前确保 snapshot 与 manifest 一致（避免 daemon 启动 rebuild 超 15s）
    _ensure_snapshot_fresh(OUTFIT_AGENT, PROJECT_ROOT / "data" / "template_registry.json")

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

    # LLM 调用（精确匹配 [llm_call] 行的响应延迟，避免误匹配
    # route latency=18ms / create_latency=439ms 等其他 latency 字段）
    llm_latencies = [
        int(x) for x in re.findall(r"\[llm_call\].*?<<<.*?latency=(\d+)ms", text)
    ]

    # 工具执行（v2 同一 exec 打印两行：带 sandbox_mode / 带 args，duration 差 0.1ms；
    # 按 (name, int(duration)) 截断去重合并为同一次执行——对齐 aggregate_trace_logs.py 口径）
    tool_execs: dict[str, list[float]] = {}
    seen: set[tuple[str, int]] = set()
    for m in re.finditer(r"tool_exec name=(\w+) duration_ms=([\d.]+)", text):
        name, dur = m.group(1), float(m.group(2))
        key = (name, int(dur))
        if key in seen:
            continue
        seen.add(key)
        tool_execs.setdefault(name, []).append(dur)

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
        # 同一对话的澄清回复必须复用同一 client_id（SessionRouter 按
        # user_id:client_id 路由同一 dialogue）——轮2/轮3 是轮1 推荐后的
        # 澄清回答（地点/场合/风格 → 性别/体型/身高），不是独立新请求。
        dialogue_client_id = f"perf-baseline-dialogue-{int(time.time() * 1000)}"
        for i, question in enumerate(STANDARD_QUESTIONS, 1):
            task_id = f"perf-baseline-{i}-{int(time.time() * 1000)}"
            t0 = time.monotonic()
            _send_message("127.0.0.1", port, question, task_id,
                          client_id=dialogue_client_id)
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
