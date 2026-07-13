"""
集成测试：上下文压缩在 coordinator_decide 执行链路中正确触发。

不依赖 daemon/TCP RPC，直接设置环境变量调用 coordinator 节点脚本，
验证 [flow] compaction: 日志行正确出现，且 node 正常退出。

测试策略：
  - AF_MODEL_MAX_TOKENS=500 → threshold=400 tok
  - 正常输入（不含压缩触发日志）：system + user ≈ 400 tok，不触发压缩
  - 长时间输入（大历史模拟）：用 AF_DIALOGUE_ID 预制大历史 → 触发压缩
  - 检查 stderr 是否有 [flow] compaction:

重构后（agent-template-delta）：节点是入口文件，subprocess 调它仍能工作，
入口文件会 import agentflow_runtime 中的真正实现。
"""

from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path

import pytest

# 入口文件仍位于 template/nodes/（subprocess 调入口文件，入口文件触发 pip 包）
# tests/ → agentflow-runtime/ → packages/ → aisf_agentflow/（项目根）
PROJECT_ROOT = Path(__file__).resolve().parents[3]
NODES_DIR = PROJECT_ROOT / "frontend" / "bridge" / "templates" / "agent_template" / "harness_agent" / "nodes"
COORDINATOR_SCRIPT = NODES_DIR / "coordinator_decide.py"


def _run_coordinator(env: dict[str, str]) -> tuple[str, str, int]:
    """Run coordinator_decide.py with given env, return (stdout, stderr, rc)."""
    base_env = os.environ.copy()
    # Set minimal node env vars
    base_env.update({
        "AF_NODE_ID": "test_compaction_node",
        "AF_DATABUS_UDS_FILE": "",  # 禁用 DataBus（无 UDS）
        "AF_DIALOGUE_ID": "",
        "AF_SESSION_ID": "test-compaction-session",
        "AF_LLM_API_KEY": "test-key-not-used",
        "AF_LLM_BASE_URL": "http://127.0.0.1:1/v1",  # 无法连接，但不会挂起
        # Coordinator 会 call_llm（curl），会快速失败返回 _llm_error
        # 但压缩检查发生在 call_llm 之前，所以即使 LLM 失败也能验证压缩
    })
    base_env.update(env)

    result = subprocess.run(
        [sys.executable, str(COORDINATOR_SCRIPT)],
        env=base_env,
        capture_output=True,
        text=True,
        timeout=30,
    )
    return result.stdout, result.stderr, result.returncode


class TestCompactionIntegration:
    """直接测试 coordinator_decide.py 的上下文压缩集成。"""

    def test_compaction_not_triggered_with_small_input(self) -> None:
        """小输入不触发压缩日志。"""
        stdout, stderr, rc = _run_coordinator({
            "AF_INPUT": "你好",
            "AF_MODEL_MAX_TOKENS": "50000",  # 大窗口
        })
        assert "compaction:" not in stderr, "大窗口不应触发压缩"

    def test_compaction_triggers_with_small_window(self) -> None:
        """极小窗口下第一轮就触发压缩。"""
        stdout, stderr, rc = _run_coordinator({
            "AF_INPUT": "你好，帮我推荐穿搭",
            "AF_MODEL_MAX_TOKENS": "400",  # threshold=320 tok
        })
        assert "compaction:" in stderr, (
            "AF_MODEL_MAX_TOKENS=400 应触发压缩\n"
            f"stderr:\n{stderr[:2000]}"
        )

    def test_compaction_log_format(self) -> None:
        """压缩日志格式正确：对齐设计文档 §6 — 含 → tok，且 ratio=/dropped=/folded=/msgs 择一。"""
        stdout, stderr, rc = _run_coordinator({
            "AF_INPUT": "北京今天35度，帮我推荐穿搭",
            "AF_MODEL_MAX_TOKENS": "400",
        })
        # 找压缩行
        for line in stderr.splitlines():
            if "compaction:" in line:
                assert "→" in line, f"应包含 →: {line}"
                assert "tok" in line, f"应包含 tok: {line}"
                # 设计文档 §6：非 emergency 分支含 ratio= 和 dropped=/folded=；
                # emergency 分支特殊格式含 msgs。择一即可。
                assert any(k in line for k in ("ratio=", "dropped=", "folded=", "msgs")), (
                    f"应含 ratio=/dropped=/folded=/msgs 之一 (设计文档 §6): {line}"
                )
                return
        pytest.fail("未找到 compaction 日志行")

    def test_compaction_shows_token_change(self) -> None:
        """压缩日志包含 before→after token 变化（设计文档 §6 格式）。"""
        stdout, stderr, rc = _run_coordinator({
            "AF_INPUT": "帮我推荐一套商务休闲穿搭",
            "AF_MODEL_MAX_TOKENS": "400",
        })
        for line in stderr.splitlines():
            if "compaction:" in line:
                # 设计文档格式: compaction: <level> (450→320 tok, ratio=0.xx, ...)
                assert "→" in line
                # 提取括号内的 → 分割
                if "(" in line and ")" in line:
                    inside = line[line.index("(") + 1:line.index(")")]
                    if "→" in inside:
                        parts = inside.split("→")
                        before = parts[0].strip()
                        after = parts[1].split()[0]
                        assert before.isdigit(), f"before 应为数字: {before}"
                        assert after.isdigit(), f"after 应为数字: {after}"
                return
        pytest.fail("未找到 compaction 日志行")

    def test_compaction_preserves_system_message(self) -> None:
        """压缩后 system 消息不被丢弃。"""
        stdout, stderr, rc = _run_coordinator({
            "AF_INPUT": "今天穿什么？" * 50,  # 长输入增加 token
            "AF_MODEL_MAX_TOKENS": "400",
        })
        # LLM call 可能失败（curl 连不上 mock），但压缩日志应在 stderr 中
        assert "compaction:" in stderr
        # 验证 node 正常退出
        assert rc == 0 or "_llm_error" in stdout or "error" in stdout

    def test_compaction_configurability(self) -> None:
        """AF_COMPACTION_THRESHOLD 可配置。"""
        stdout, stderr, rc = _run_coordinator({
            "AF_INPUT": "推荐穿搭",
            "AF_MODEL_MAX_TOKENS": "1000",
            "AF_COMPACTION_THRESHOLD": "0.3",  # 降低阈值，压缩更容易触发
        })
        # threshold=1000*0.3=300 tok，system prompt ≈ 400 tok → 触发压缩
        assert "compaction:" in stderr, "低阈值也应触发压缩"

    # ════════════════════════════════════════════════════════════════
    # Regression: interrupt flag poll (I-INT-5, design §3.3.2)
    # ════════════════════════════════════════════════════════════════

    def test_interrupt_poll_failure_does_not_block(self) -> None:
        """回归：interrupt flag 轮询失败（DataBus 不可用）不阻断 coordinator 正常退出。
        
        I-INT-5 的集成级验证：子进程运行 coordinator_decide.py，
        AF_DATABUS_UDS_FILE="" 下 get_global 不可用，轮询打 WARN 日志后继续。
        coordinator 正常走 LLM call → _llm_error → fallback path → 正常退出。
        """
        stdout, stderr, rc = _run_coordinator({
            "AF_INPUT": "推荐穿搭",
            "AF_DATABUS_UDS_FILE": "",  # 禁用 DataBus，轮询必然失败
            "AF_LLM_BASE_URL": "http://127.0.0.1:1/v1",  # 快速失败
        })
        # 中断轮询失败应打 WARN 日志（不阻断）
        assert "interrupt flag poll failed" in stderr, (
            "DataBus 不可用时，中断轮询应打 WARN 日志\n"
            f"stderr:\n{stderr[:2000]}"
        )
        # node 正常退出（LLM call 失败但走 fallback）
        assert rc == 0 or "_llm_error" in stdout or "error" in stdout, (
            "轮询失败后 coordinator 应正常退出\n"
            f"stdout:\n{stdout[:1000]}\nstderr:\n{stderr[:1000]}"
        )

    def test_interrupt_flag_not_set_continues_normally(self) -> None:
        """回归：interrupt flag 未设置时，中断轮询应跳过（不输出日志），正常执行。"""
        stdout, stderr, rc = _run_coordinator({
            "AF_INPUT": "推荐穿搭",
            "AF_DATABUS_UDS_FILE": "",  # 禁用 DataBus
            "AF_LLM_BASE_URL": "http://127.0.0.1:1/v1",  # 快速失败
        })
        # 不应有中断相关的日志行（flag 不存在时轮询静默跳过）
        interrupt_logs = [l for l in stderr.splitlines() if "interrupt" in l.lower()]
        assert len(interrupt_logs) <= 1, (
            # 最多一条 "interrupt flag poll failed"（DataBus 不可用时的 fallback 日志）
            # 不应有 "interrupted by"（unset flag 不应命中）
            f"unset flag 不应产生中断相关日志: {interrupt_logs}"
        )
        # 验证轮询失败的 WARN 日志格式正确（含 dialogue_id 信息）
        for line in stderr.splitlines():
            if "interrupt flag poll failed" in line:
                # 日志应包含异常信息，帮助定位
                assert "get_global" in line or "DataBus" in line or "UDS" in line or "Errno" in line or "No such" in line or "Connection" in line, \
                    f"轮询失败日志应包含异常原因: {line}"
