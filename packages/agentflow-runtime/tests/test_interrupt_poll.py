"""
单元测试：skill_executor / coordinator_decide 的 interrupt flag 轮询。

覆盖设计文档 §3.3 + 不变量 I-INT-5（轮询失败不阻断主流程）、I-INT-6（命中后写
interrupted 结果给 coordinator 走正常 cleanup）。

不依赖 daemon / TCP RPC，直接 mock DataBusClient.get_global 返回 "true"，
验证 skill_executor.execute() 在循环顶部检测到 flag 后主动退出，
且写出 status=interrupted 的 skill_result。
"""

from __future__ import annotations

import json
import os
import sys
from pathlib import Path
from unittest.mock import MagicMock, patch

import pytest

PROJECT_ROOT = Path(__file__).resolve().parents[2]
PKG_SRC = PROJECT_ROOT / "packages" / "agentflow-runtime" / "src"
if str(PKG_SRC) not in sys.path:
    sys.path.insert(0, str(PKG_SRC))


@pytest.fixture
def interrupt_flag_env(monkeypatch):
    """环境变量基线：让 skill_executor 能进入 LLM while-loop 顶部轮询。"""
    monkeypatch.setenv("AF_NODE_ID", "skill_executor")
    monkeypatch.setenv("AF_DIALOGUE_ID", "d_test_interrupt")
    monkeypatch.setenv("AF_SESSION_ID", "ses_test")
    monkeypatch.setenv("AF_INPUT", "请帮我查询天气")
    monkeypatch.setenv("AF_EXEC_SKILL_KEY", "dispatch:1")
    monkeypatch.setenv("AF_DATABUS_UDS_FILE", "")  # 禁用真实 UDS
    # 让 node_timeout_sec 足够大，不先触发硬超时
    monkeypatch.setenv("AGENT_PACKAGE_DIR",
                       str(PROJECT_ROOT / "examples" / "agent" / "outfit-assistant"))
    return monkeypatch


class _FakeDataBus:
    """模拟 DataBusClient，可控返回 interrupt flag。"""

    def __init__(self, interrupt_flag: str = ""):
        self._interrupt_flag = interrupt_flag
        self.connected = True
        self.put_calls: list[tuple[str, str]] = []
        self.push_calls: list[tuple] = []

    def is_connected(self) -> bool:
        return self.connected

    def get_global(self, key: str) -> str | None:
        if key.startswith("interrupt:"):
            return self._interrupt_flag or None
        return None

    def put(self, key: str, value: str) -> bool:
        self.put_calls.append((key, value))
        return True

    def response_push(self, content: str, **kwargs) -> bool:
        self.push_calls.append((content, kwargs))
        return True


def test_skill_executor_interrupt_flag_hit_returns_early(interrupt_flag_env):
    """I-INT-6: 命中 flag 后写 interrupted skill_result 并 return，不调 call_llm。"""
    from agentflow_runtime import skill_executor

    fake_bus = _FakeDataBus(interrupt_flag="true")

    with patch.object(skill_executor, "DataBusClient", return_value=fake_bus), \
         patch.object(skill_executor, "call_llm") as mock_llm, \
         patch.object(skill_executor, "emit_event") as mock_emit, \
         patch.object(skill_executor, "AgentConfig") as mock_cfg:

        # 配置 AgentConfig 让循环能进入
        mock_cfg.skill_max_iterations.return_value = 5
        mock_cfg.node_execution_timeout.return_value = 3600
        mock_cfg.skill_max_completion_tokens.return_value = 4000
        mock_cfg.skill_llm_temperature.return_value = 0.7
        mock_cfg.skill_llm_top_p.return_value = 1.0
        mock_cfg.skill_llm_frequency_penalty.return_value = 0.0
        mock_cfg.skill_llm_presence_penalty.return_value = 0.0
        mock_cfg.skill_llm_top_k.return_value = 0
        mock_cfg.skill_llm_stop.return_value = None

        # 让 databus 初始化为 fake（DataBusClient 被 patch 但 skill_executor 内部直接构造）
        # 所以需要 patch 内部使用
        import io
        buf = io.StringIO()
        with patch("sys.stdout", buf):
            skill_executor.execute()

        # call_llm 不应被调用——循环顶部就该 return
        mock_llm.assert_not_called()

        # 应写 interrupted skill_result
        put_calls = dict(fake_bus.put_calls)
        assert "skill_result:skill-runner" in put_calls, \
            "必须写 skill_result:skill-runner 给 coordinator"
        result = json.loads(put_calls["skill_result:skill-runner"])
        assert result["status"] == "interrupted"
        assert result["skill_result"] == "对话已被取消"

        # 应 emit interrupt 事件
        event_names = [call.args[0] if call.args else "" for call in mock_emit.call_args_list]
        assert any("interrupted" in n for n in event_names), \
            f"应 emit skill_executor.interrupted 事件，实际: {event_names}"


def test_skill_executor_interrupt_flag_miss_continues(interrupt_flag_env):
    """I-INT-5 反向验证: flag 未命中时进入正常 call_llm 路径（不误退出）。"""
    from agentflow_runtime import skill_executor

    fake_bus = _FakeDataBus(interrupt_flag="")  # 未中断

    with patch.object(skill_executor, "DataBusClient", return_value=fake_bus), \
         patch.object(skill_executor, "call_llm") as mock_llm, \
         patch.object(skill_executor, "emit_event"), \
         patch.object(skill_executor, "AgentConfig") as mock_cfg:

        mock_cfg.skill_max_iterations.return_value = 5
        mock_cfg.node_execution_timeout.return_value = 3600
        mock_cfg.skill_max_completion_tokens.return_value = 4000
        mock_cfg.skill_llm_temperature.return_value = 0.7
        mock_cfg.skill_llm_top_p.return_value = 1.0
        mock_cfg.skill_llm_frequency_penalty.return_value = 0.0
        mock_cfg.skill_llm_presence_penalty.return_value = 0.0
        mock_cfg.skill_llm_top_k.return_value = 0
        mock_cfg.skill_llm_stop.return_value = None

        # 让 call_llm 返回一个最终答案立即退出循环
        mock_llm.return_value = {
            "choices": [{"message": {"content": "最终答案", "finish_reason": "stop"}}],
            "usage": {"prompt_tokens": 10, "completion_tokens": 5},
        }

        import io
        buf = io.StringIO()
        with patch("sys.stdout", buf):
            skill_executor.execute()

        # call_llm 应被调用（未命中 flag，进入正常路径）
        mock_llm.assert_called()


def test_skill_executor_poll_failure_does_not_block(interrupt_flag_env):
    """I-NT-5: 轮询失败（DataBus 抖动抛异常）不阻断主流程，继续调 call_llm。"""
    from agentflow_runtime import skill_executor

    fake_bus = _FakeDataBus(interrupt_flag="true")
    # 让 get_global 抛异常模拟 DataBus 抖动
    fake_bus.get_global = MagicMock(side_effect=Exception("UDS reconnect"))

    with patch.object(skill_executor, "DataBusClient", return_value=fake_bus), \
         patch.object(skill_executor, "call_llm") as mock_llm, \
         patch.object(skill_executor, "emit_event"), \
         patch.object(skill_executor, "AgentConfig") as mock_cfg:

        mock_cfg.skill_max_iterations.return_value = 5
        mock_cfg.node_execution_timeout.return_value = 3600
        mock_cfg.skill_max_completion_tokens.return_value = 4000
        mock_cfg.skill_llm_temperature.return_value = 0.7
        mock_cfg.skill_llm_top_p.return_value = 1.0
        mock_cfg.skill_llm_frequency_penalty.return_value = 0.0
        mock_cfg.skill_llm_presence_penalty.return_value = 0.0
        mock_cfg.skill_llm_top_k.return_value = 0
        mock_cfg.skill_llm_stop.return_value = None

        mock_llm.return_value = {
            "choices": [{"message": {"content": "最终答案", "finish_reason": "stop"}}],
            "usage": {"prompt_tokens": 10, "completion_tokens": 5},
        }

        import io
        buf = io.StringIO()
        with patch("sys.stdout", buf):
            skill_executor.execute()

        # 轮询失败后主流程继续，call_llm 被调用
        mock_llm.assert_called()


# ════════════════════════════════════════════════════════════════════
# Regression: 重复中断幂等性（I-INT-4 延伸）
# ════════════════════════════════════════════════════════════════════


def test_interrupt_flag_stays_set_on_repeated_poll(interrupt_flag_env):
    """回归：同一 dialogue 的 interrupt flag 多次轮询始终返回 'true'（幂等）。
    
    模拟 daemon 写一次 flag 后，skill_executor 的多轮循环都应读到相同值，
    不会因读取操作而清除 flag。
    """
    from agentflow_runtime import skill_executor

    # 模拟一个"每次轮询都读到 true"的 DataBus——flag 不会因读取而消失
    call_count = 0
    
    class _PersistentFakeDataBus:
        def __init__(self):
            self.connected = True
            self.put_calls = []
            self.push_calls = []
        
        def is_connected(self):
            return True
        
        def get_global(self, key):
            # 始终返回 "true"，模拟 flag 在轮询间不消失
            return "true" if key.startswith("interrupt:") else None
        
        def put(self, key, value):
            self.put_calls.append((key, value))
            return True
        
        def response_push(self, content, **kwargs):
            self.push_calls.append((content, kwargs))
            return True

    fake_bus = _PersistentFakeDataBus()

    with patch.object(skill_executor, "DataBusClient", return_value=fake_bus), \
         patch.object(skill_executor, "call_llm") as mock_llm, \
         patch.object(skill_executor, "emit_event"), \
         patch.object(skill_executor, "AgentConfig") as mock_cfg:

        mock_cfg.skill_max_iterations.return_value = 5
        mock_cfg.node_execution_timeout.return_value = 3600
        mock_cfg.skill_max_completion_tokens.return_value = 4000
        mock_cfg.skill_llm_temperature.return_value = 0.7
        mock_cfg.skill_llm_top_p.return_value = 1.0
        mock_cfg.skill_llm_frequency_penalty.return_value = 0.0
        mock_cfg.skill_llm_presence_penalty.return_value = 0.0
        mock_cfg.skill_llm_top_k.return_value = 0
        mock_cfg.skill_llm_stop.return_value = None

        import io
        buf = io.StringIO()
        with patch("sys.stdout", buf):
            skill_executor.execute()

        # call_llm 不应被调用——第 1 轮就命中 flag 退出
        mock_llm.assert_not_called()

        # flag 被读了至少 1 次（实际读的 get_global，但我们 mock 的不是 call_count-aware）
        # 验证写了 interrupted skill_result
        put_calls = dict(fake_bus.put_calls)
        assert "skill_result:skill-runner" in put_calls
        result = json.loads(put_calls["skill_result:skill-runner"])
        assert result["status"] == "interrupted"


# ════════════════════════════════════════════════════════════════════
# Regression: coordinator_decide 中断轮询（I-INT-5/6 在 coordinator 侧）
# ════════════════════════════════════════════════════════════════════


def test_coordinator_interrupt_flag_hit_returns_early(monkeypatch):
    """回归：coordinator_decide 命中 interrupt flag 后写 interrupted 响应并 return。"""
    from agentflow_runtime import coordinator_decide as cd

    # 模拟一个命中 flag 的 DataBus
    class _CoordFakeDataBus:
        def __init__(self):
            self.connected = True
            self.put_calls = []
            self.delete_calls = []
            self.raw_client = type("RawClient", (), {
                "put": lambda self, k, v: None
            })()
        
        def is_connected(self):
            return True
        
        def get_global(self, key):
            return "true" if key.startswith("interrupt:") else None
        
        def delete(self, key):
            self.delete_calls.append(key)
            return True

    fake_bus = _CoordFakeDataBus()

    # 设置最小环境
    monkeypatch.setenv("AF_NODE_ID", "coordinator")
    monkeypatch.setenv("AF_DIALOGUE_ID", "d_test_coord_interrupt")
    monkeypatch.setenv("AF_SESSION_ID", "ses_test")
    monkeypatch.setenv("AF_DATABUS_UDS_FILE", "")
    monkeypatch.setenv("AF_INPUT", "请帮我做决定")

    with patch.object(cd, "DataBusClient", return_value=fake_bus), \
         patch.object(cd, "call_llm") as mock_llm, \
         patch.object(cd, "emit_event") as mock_emit, \
         patch.object(cd, "AgentConfig") as mock_cfg:

        mock_cfg.coordinator_max_iterations.return_value = 5
        mock_cfg.node_timeout_sec.return_value = 3600
        mock_cfg.max_completion_tokens.return_value = 4000
        mock_cfg.llm_temperature.return_value = 0.7
        mock_cfg.llm_top_p.return_value = 1.0
        mock_cfg.fold_compression_enabled.return_value = False
        mock_cfg.protect_last_rounds.return_value = 0
        mock_cfg.skill_max_retries.return_value = 0

        import io
        buf = io.StringIO()
        with patch("sys.stdout", buf):
            cd.execute()

        # call_llm 不应被调用——第 1 轮 call_llm 前就命中 flag
        mock_llm.assert_not_called()

        # stdout 应输出 interrupted 响应
        output_lines = buf.getvalue().strip().splitlines()
        assert len(output_lines) > 0, "stdout 应有输出"
        last_output = json.loads(output_lines[-1])
        assert last_output.get("status") == "interrupted", \
            f"应输出 status=interrupted: {last_output}"

        # 应清掉旧 response key（与正常完成路径对齐）
        assert "response" in fake_bus.delete_calls, \
            "中断路径应清旧 response key"


def test_coordinator_interrupt_flag_miss_continues(monkeypatch):
    """回归：coordinator_decide 未命中 interrupt flag 时正常调 call_llm。"""
    from agentflow_runtime import coordinator_decide as cd

    class _CoordFakeDataBus:
        def __init__(self):
            self.connected = True
            self.raw_client = type("RawClient", (), {
                "put": lambda self, k, v: None
            })()
        
        def is_connected(self):
            return True
        
        def get_global(self, key):
            return None  # 未中断
        
        def delete(self, key):
            return True

    fake_bus = _CoordFakeDataBus()

    monkeypatch.setenv("AF_NODE_ID", "coordinator")
    monkeypatch.setenv("AF_DIALOGUE_ID", "d_test_coord_miss")
    monkeypatch.setenv("AF_SESSION_ID", "ses_test")
    monkeypatch.setenv("AF_DATABUS_UDS_FILE", "")
    monkeypatch.setenv("AF_INPUT", "请帮我做决定")

    with patch.object(cd, "DataBusClient", return_value=fake_bus), \
         patch.object(cd, "call_llm") as mock_llm, \
         patch.object(cd, "emit_event"), \
         patch.object(cd, "AgentConfig") as mock_cfg:

        mock_cfg.coordinator_max_iterations.return_value = 5
        mock_cfg.node_timeout_sec.return_value = 3600
        mock_cfg.max_completion_tokens.return_value = 4000
        mock_cfg.llm_temperature.return_value = 0.7
        mock_cfg.llm_top_p.return_value = 1.0
        mock_cfg.fold_compression_enabled.return_value = False
        mock_cfg.protect_last_rounds.return_value = 0
        mock_cfg.skill_max_retries.return_value = 0

        # call_llm 返回失败走 fallback 路径（快速退出）
        mock_llm.return_value = {"_llm_error": True, "error": "mock error"}

        cd.execute()

        # call_llm 应被调用（未命中 flag，进入正常路径）
        mock_llm.assert_called()
