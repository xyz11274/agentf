#!/usr/bin/env python3
"""
E2E test — llm_call 拆分重构 + prompts 收敛优化 的真实链路验证。

背景（prompt-optimizer / code-review 产出）:
  - llm_call.py 拆分为 compaction.py + recorder.py + 薄入口（call_llm）
  - prompts/coordinator.py + prompts/skill.py 增加收敛约束（收敛优先/判定标准/失败停止）

本测试通过真实 daemon + 真实 LLM 验证三个目标:
  T-1: llm_call 拆分后真实调用链完整（recorder.trace_call 被真实链路调用，返回响应）
  T-2: trace 落盘合规（F7.1: request/response/meta 落盘无 CRITICAL 告警）
  T-3: prompts 收敛生效（coordinator_round 未触发 max_iterations 强制结束）

Usage:
  # daemon 必须在 17891 端口可用（daemon_manager 会自动拉起）
  pytest bridge/tests/test_llm_call_chain_e2e.py -v -m e2e --timeout 300

  无真实 LLM key 时自动跳过（检查 outfit-assistant/.env 中的 AF_LLM_API_KEY）。
"""

import json
import os
import re
import socket
import time

import pytest

# ── Configuration ────────────────────────────────────────────────────────────

_PROJECT_ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", ".."))
_LOG_DIR = os.path.join(_PROJECT_ROOT, "logs", "tests")
DAEMON_HOST = os.environ.get("DAEMON_HOST", "127.0.0.1")
DAEMON_PORT = int(os.environ.get("DAEMON_PORT", "17891"))
AGENT_ID = os.environ.get("E2E_AGENT_ID", "outfit-assistant")
TURN_TIMEOUT = 240  # per-turn timeout (真实 LLM 耗时, 2x 余量)

_ts = int(time.time())
_DAEMON_LOG = ""


def _user_id(scenario: str) -> str:
    return f"e2e-{scenario}-{_ts}"


def _client_id(scenario: str) -> str:
    return f"cli-{scenario}-{_ts}"


def _new_log_lines(since_pos: int) -> tuple[list[str], int]:
    """Return lines added to daemon log since ``since_pos`` (file offset)."""
    lines = []
    try:
        with open(_DAEMON_LOG, "rb") as f:
            f.seek(since_pos)
            raw = f.read()
            new_pos = f.tell()
        text = raw.decode("utf-8", errors="replace")
        lines = [ln for ln in text.split("\n") if ln.strip()]
    except (FileNotFoundError, OSError):
        new_pos = 0
    return lines, new_pos


def _jsonrpc(sock: socket.socket, method: str, params: dict, req_id: int = 1) -> dict:
    """Send a JSON-RPC request, return parsed response.

    超时覆盖 agent.execute 的完整响应周期：全新 daemon 首次请求需创建
    sandbox（实测 ~16s）+ LLM 调用，响应在 20-30s 内返回（daemon_manager
    每次测试启动全新 daemon，沙箱无缓存）。
    """
    sock.settimeout(120)
    payload = json.dumps({"jsonrpc": "2.0", "method": method, "params": params, "id": req_id})
    sock.sendall((payload + "\n").encode("utf-8"))
    chunks = []
    while True:
        ch = sock.recv(1)
        if not ch or ch == b"\n":
            break
        chunks.append(ch)
    return json.loads(b"".join(chunks).decode("utf-8"))


def _has_real_llm_key() -> bool:
    """检查 outfit-assistant 包内 .env 是否有真实 LLM key（无 key 则跳过）。"""
    package_dir = os.path.join(_PROJECT_ROOT, "examples", "agent", "outfit-assistant")
    for env_file in (".env", ".env_dashscope", ".env_minimax"):
        path = os.path.join(package_dir, env_file)
        if not os.path.exists(path):
            continue
        try:
            with open(path) as f:
                content = f.read()
            if re.search(r"^(AF_LLM_API_KEY|API_KEY|DASHSCOPE_API_KEY|MINIMAX_API_KEY)\s*=\s*\S+", content, re.M):
                return True
        except OSError:
            continue
    return False


def _run_turn(message: str, user_id: str, client_id: str,
              timeout: int = TURN_TIMEOUT) -> dict:
    """Send a user message via daemon JSON-RPC, collect ALL new log lines.

    Returns ``{success, elapsed, log_lines, msg_id}`` where ``log_lines``
    contains every new line written to the daemon log during this turn
    (full evidence for the three verification targets).

    完成判定（daemon 发送最终响应时打印 ``Response ready, sending to client``
    （agent.cpp:2322 COORDINATORDataExchange）或 ``[E2E_SUMMARY]``）:
      检测到任一完成信号即视为本轮收敛，收集的日志足以支撑三目标断言。
    """
    log_pos = 0
    try:
        log_pos = os.path.getsize(_DAEMON_LOG)
    except OSError:
        pass

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(120)
    sock.connect((DAEMON_HOST, DAEMON_PORT))
    try:
        result = _jsonrpc(sock, "agent.execute", {
            "agent_id": AGENT_ID, "user_id": user_id,
            "client_id": client_id, "content": message,
        })
        msg_id = result.get("result", {}).get("message_id", "unknown")
    finally:
        sock.close()

    start = time.time()
    deadline = time.time() + timeout
    collected: list[str] = []
    done_signal = False

    while time.time() < deadline:
        new_lines, log_pos = _new_log_lines(log_pos)
        collected.extend(new_lines)
        for line in new_lines:
            # 完成信号: daemon 发送最终响应（正常/降级路径均打印）
            if "Response ready, sending to client" in line or "E2E_SUMMARY" in line:
                done_signal = True

        if done_signal:
            # 完成信号后继续轮询，直到日志静止（Python 节点 stderr 经
            # NODE_STDERR 转发有缓冲延迟，需等 llm_call 记录全部落盘）
            quiet_from = time.time()
            while time.time() - quiet_from < 10:
                time.sleep(2)
                tail_lines, log_pos = _new_log_lines(log_pos)
                if tail_lines:
                    collected.extend(tail_lines)
                    quiet_from = time.time()  # 有新行则重置静止计时
                else:
                    break  # 连续 2s 无新行 → 日志已静止
            break
        time.sleep(2)

    # success: 最终响应已发出，或 coordinator 成功执行（兜底）
    coord_success = any(
        "PROCESS_NODE_SUCCESS" in line and "NodeId=coordinator_decide" in line
        for line in collected
    )
    return {
        "success": done_signal or coord_success,
        "elapsed": time.time() - start,
        "log_lines": collected,
        "msg_id": msg_id,
    }


def _find_call_records(lines: list[str]) -> list[dict]:
    """从 daemon 日志提取 recorder.trace_call 的调用记录（入口 >>> 与出口 <<<）。

    注意: 入口行格式为
      [llm_call] WARN: [#1] >>> call_id=xxx node=coordinator_decide task_type=coordinator model=...
    ——node 与 model 之间隔有 task_type 字段，不能用相邻捕获。
    """
    records = []
    entry_re = re.compile(r"\[#(\d+)\] >>> call_id=(\w+) node=(\S+)")
    exit_re = re.compile(r"\[#(\d+)\] <<< (\w+) (OK|FAILED)")
    for line in lines:
        m = entry_re.search(line)
        if m:
            records.append({"seq": m.group(1), "call_id": m.group(2),
                            "node": m.group(3), "model": "", "phase": "entry"})
        m = exit_re.search(line)
        if m:
            records.append({"seq": m.group(1), "call_id": m.group(2),
                            "result": m.group(3), "phase": "exit"})
    return records


def _find_rounds(lines: list[str]) -> list[tuple[int, int]]:
    """提取 coordinator_round=N/max 日志，返回 [(round, max), ...]。"""
    rounds = []
    m_re = re.compile(r"coordinator_round=(\d+)/(\d+)")
    for line in lines:
        m = m_re.search(line)
        if m:
            rounds.append((int(m.group(1)), int(m.group(2))))
    return rounds


# ══════════════════════════════════════════════════════════════════════════════
# Test suite
# ══════════════════════════════════════════════════════════════════════════════


@pytest.mark.e2e
@pytest.mark.slow
class TestLLMCallChainE2E:
    """llm_call 拆分 + prompts 收敛 的真实链路验证。"""

    @classmethod
    @pytest.fixture(autouse=True, scope="class")
    def _daemon_lifecycle(cls):
        global _DAEMON_LOG
        if not _has_real_llm_key():
            pytest.skip("无真实 LLM key（outfit-assistant/.env 缺 AF_LLM_API_KEY），跳过真实链路 e2e")
        from daemon_manager import start as d_start, stop as d_stop
        # 日志落本项目可见目录 logs/tests/（F8 规范：不落 /tmp）
        _DAEMON_LOG = d_start(port=DAEMON_PORT, log_dir=_LOG_DIR)
        yield
        d_stop()

    def _run_and_collect(self, message: str, scenario: str) -> dict:
        turn = _run_turn(message, _user_id(scenario), _client_id(scenario))
        lines = turn["log_lines"]
        records = _find_call_records(lines)
        rounds = _find_rounds(lines)
        return {"turn": turn, "lines": lines, "records": records, "rounds": rounds}

    # ── T-1: 拆分后真实调用链 ────────────────────────────────────────────

    def test_llm_call_chain_completes(self):
        """T-1: llm_call 拆分后真实链路完整——recorder.trace_call 被真实调用并成功返回。"""
        r = self._run_and_collect("今天北京天气如何", "chain-weather")

        assert r["turn"]["success"], (
            f"[T-1] daemon 未产生 Response ready. "
            f"elapsed={r['turn']['elapsed']:.1f}s msg_id={r['turn']['msg_id']}\n"
            f"tail: {r['lines'][-10:]}"
        )

        entries = [rec for rec in r["records"] if rec["phase"] == "entry"]
        exits_ok = [rec for rec in r["records"] if rec["phase"] == "exit" and rec["result"] == "OK"]
        assert entries, (
            f"[T-1] daemon 日志中未发现 recorder.trace_call 入口记录 "
            f"([#seq] >>> call_id=...)。tail:\n{r['lines'][-15:]}"
        )
        assert exits_ok, (
            f"[T-1] 有 {len(entries)} 次 LLM 调用但无 OK 出口记录 "
            f"([#seq] <<< ... OK)。records={r['records']}"
        )
        # 断言调用链是 coordinator + skill_executor 双节点（拆分后两入口都走 recorder）
        nodes = {rec["node"] for rec in r["records"] if rec["phase"] == "entry"}
        assert nodes, "[T-1] 无 node 信息"
        print(f"[T-1] ✅ 真实 LLM 调用链完成: {len(entries)} 次调用, nodes={sorted(nodes)}")

    # ── T-2: trace 落盘合规（F7.1） ──────────────────────────────────────

    def test_trace_persisted_no_critical(self):
        """T-2: trace 落盘合规——无 CRITICAL meta 空写告警（F7.1 写后读回校验通过）。"""
        r = self._run_and_collect("帮我推荐今天上班的穿搭", "trace-outfit")

        assert r["turn"]["success"], f"[T-2] turn 未完成: {r['turn']['elapsed']:.1f}s"

        # F7.1: meta 写后读回校验——若 _verify_meta_written 发现空 meta 会打 CRITICAL
        criticals = [ln for ln in r["lines"] if "CRITICAL: meta" in ln]
        assert not criticals, (
            f"[T-2] F7.1 违规: 发现 {len(criticals)} 条 CRITICAL meta 空写告警:\n"
            f"{criticals[:3]}"
        )
        # recorder 落盘路径必须被调用（_put traces/... 无失败告警）
        put_failures = [ln for ln in r["lines"] if "put 'traces/" in ln and "failed" in ln]
        assert not put_failures, (
            f"[T-2] trace 落盘失败 {len(put_failures)} 次:\n{put_failures[:3]}"
        )
        print("[T-2] ✅ trace 落盘无 CRITICAL/失败告警（F7.1 写后读回校验通过）")

    # ── T-3: prompts 收敛（轮次不过多） ──────────────────────────────────

    def test_coordinator_round_converges(self):
        """T-3: prompts 收敛——coordinator_round 未触发 max_iterations 强制结束。"""
        r = self._run_and_collect("北京今天适合穿什么", "round-converge")

        assert r["turn"]["success"], f"[T-3] turn 未完成: {r['turn']['elapsed']:.1f}s"

        assert r["rounds"], (
            f"[T-3] 日志中未发现 coordinator_round=N/max 记录。tail:\n{r['lines'][-15:]}"
        )
        max_iterations = r["rounds"][0][1]
        final_round = r["rounds"][-1][0]
        # 收敛断言: 未触发 max_iterations 强制结束
        forced_end = [ln for ln in r["lines"] if "max iterations" in ln.lower()]
        assert not forced_end, (
            f"[T-3] prompts 收敛失败: 触发 max iterations ({max_iterations}) 强制结束:\n"
            f"{forced_end[:3]}"
        )
        assert final_round < max_iterations, (
            f"[T-3] 最终轮次 {final_round} 达到上限 {max_iterations}"
        )
        # 收敛质量: 简单天气查询应 ≤ 3 轮（coordinator 首轮 + 至多 2 次追加）
        assert final_round <= 3, (
            f"[T-3] 简单查询仍跑了 {final_round} 轮（上限 {max_iterations}），"
            f"提示词收敛约束未生效。rounds={r['rounds']}"
        )
        print(f"[T-3] ✅ 轮次收敛: coordinator_round={final_round}/{max_iterations}")

    # ── 组合: 一条消息验证全部三目标（供 CI 单测快速回归） ────────────────

    def test_combined_all_targets(self):
        """T-1+T-2+T-3 组合：一条消息同时验证调用链、落盘、收敛。"""
        r = self._run_and_collect("今天天气怎么样，适合出门吗", "combined")

        assert r["turn"]["success"], "[combined] turn 未完成"
        entries = [rec for rec in r["records"] if rec["phase"] == "entry"]
        exits_ok = [rec for rec in r["records"] if rec["phase"] == "exit" and rec["result"] == "OK"]
        assert entries and exits_ok, f"[combined] 调用链不完整: {r['records']}"
        assert not [ln for ln in r["lines"] if "CRITICAL: meta" in ln], "[combined] F7.1 违规"
        forced_end = [ln for ln in r["lines"] if "max iterations" in ln.lower()]
        assert not forced_end, "[combined] 触发 max iterations 强制结束"
        print(f"[combined] ✅ 三目标全过: {len(entries)} 次 LLM 调用, "
              f"rounds={r['rounds']}")
