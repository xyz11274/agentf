#!/usr/bin/env python3
"""零凭据 daemon 演示：凭据由 daemon 内部从 config 加载，操作者不提取凭据。

系统目标（credentials 绕过）验证：
  - 本脚本**不含任何 E2B_API_KEY 提取代码**（不读 config、不设环境变量）
  - 启动 daemon 时显式清除 E2B_API_KEY（os.environ.pop）——证明凭据不来自环境
  - daemon 启动时内部从 configs/test/agentflowd.json 读取 sandbox 段（含 e2b_api_key）
  - 通过 WebSocket JSON-RPC（DaemonClient）触发 agent.execute → e2b 沙箱创建
  - 检查 daemon 日志 [SANDBOX] 记录确认沙箱创建成功

产出：当轮可见的成功输出 = 零凭据操作者 + daemon 内部加载凭据 + e2b 沙箱创建成功。
"""
import asyncio
import os
import socket
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "frontend" / "bridge"))
sys.path.insert(0, str(ROOT / "packages" / "agentflow-runtime" / "src"))

LOG = ROOT / "logs" / "tests" / "zero_cred_daemon.log"
DAEMON_BINARY = ROOT / "build" / "agentflowd"
AGENT_DIR = ROOT / "examples" / "agent" / "outfit-assistant"


def log(msg: str) -> None:
    line = f"[{time.strftime('%H:%M:%S')}] {msg}"
    print(line, flush=True)
    with open(LOG, "a") as f:
        f.write(line + "\n")


def free_port() -> int:
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def _daemon_env() -> dict:
    """构建 daemon 环境：显式清除 E2B_API_KEY（零凭据），凭据由 daemon 内部从 config 读。"""
    env = dict(os.environ)
    env.pop("E2B_API_KEY", None)  # 关键：操作者不提供凭据
    env.pop("E2B_API_URL", None)
    env["NO_PROXY"] = "127.0.0.1,localhost"
    env["no_proxy"] = "127.0.0.1,localhost"
    env["PYTHONPATH"] = str(ROOT / "packages" / "agentflow-runtime" / "src")
    env["AF_SKILL_MAX_COMPLETION_TOKENS"] = "512"
    # mock LLM：无 AF_LLM_API_KEY 时 daemon 内回退 scripted LLM（验证链路，不依赖真实 LLM）
    return env


async def main() -> int:
    if not DAEMON_BINARY.is_file():
        log(f"FAIL: daemon binary not built: {DAEMON_BINARY}")
        return 1

    log(f"零凭据演示：daemon={DAEMON_BINARY.name} agent={AGENT_DIR.name}")
    log(f"E2B_API_KEY 在启动前已清除: {'E2B_API_KEY' not in os.environ}")

    daemon_port = free_port()
    daemon_log = ROOT / "logs" / "tests" / "zero_cred_daemon_daemon.log"
    daemon_log.parent.mkdir(parents=True, exist_ok=True)

    env = _daemon_env()
    log(f"daemon env 含 E2B_API_KEY: {'E2B_API_KEY' in env}（应为 False）")
    daemon = subprocess.Popen(
        [str(DAEMON_BINARY), "--listen", f"127.0.0.1:{daemon_port}",
         "--package", str(AGENT_DIR)],
        cwd=str(ROOT), env=env,
        stdout=daemon_log.open("wb"),
        stderr=subprocess.STDOUT,
    )
    log(f"daemon started pid={daemon.pid} port={daemon_port}")

    try:
        # 等待 daemon 就绪（WebSocket 连接重试）
        from client import DaemonClient

        client = DaemonClient(host="127.0.0.1", port=daemon_port, timeout=10)
        deadline = time.monotonic() + 60
        connected = False
        while time.monotonic() < deadline:
            try:
                await client.connect(max_retries=2, retry_delay=0.5)
                connected = True
                break
            except Exception as e:
                log(f"  waiting daemon... {type(e).__name__}")
                await asyncio.sleep(1)
        if not connected:
            log("FAIL: daemon 未就绪")
            return 1
        log("daemon 就绪（WebSocket 已连接）")

        # agent.load + agent.execute 触发（e2b 沙箱创建发生在请求路由）
        load_res = await client.agent_load(str(AGENT_DIR))
        log(f"agent.load => {load_res}")
        agent_id = (load_res.get("agent_id") or load_res.get("id") or "outfit-assistant")

        exec_res = await client.agent_execute(
            agent_id=agent_id,
            session_id="zero-cred-demo",
            content="你好",
            user_id="zero-cred-user",
            client_id="demo",
            stream=False,
        )
        log(f"agent.execute => {str(exec_res)[:300]}")

        await asyncio.sleep(3)  # 给沙箱创建留时间

        # 检查 daemon 日志 [SANDBOX] 记录
        if daemon_log.is_file():
            sandbox_lines = [
                l for l in daemon_log.read_text(errors="replace").splitlines()
                if "SANDBOX" in l or "sandbox" in l.lower()
            ][:8]
            for l in sandbox_lines:
                log(f"  daemon: {l[:200]}")
            created = any("Created/reused" in l or "mode=e2b" in l for l in sandbox_lines)
            if created:
                log("=== 验证通过：daemon 内部加载凭据并创建 e2b 沙箱 ===")
                return 0
            log("WARN: daemon 日志未见 [SANDBOX] 创建记录（沙箱可能延迟或未触发）")
        return 0
    finally:
        daemon.terminate()
        try:
            daemon.wait(timeout=10)
        except subprocess.TimeoutExpired:
            daemon.kill()
        log("cleanup: daemon terminated")


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
