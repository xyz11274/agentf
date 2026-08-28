"""
回归测试：ask_user 不可用时的文本询问降级（提示词层面方案，2026-08-08）。

最终方案（无 coordinator 代码改动、无结构化标记、不过度设计）：
- skill_executor 系统提示词 SKILL_ASK_USER_FALLBACK_PROMPT：技能文档提到的
  ask_user/render_ui 等会话内置工具**不在工具列表**时，不要假装调用，降级为
  文本一次性询问用户，并把问题直接作为 skill_result 正文返回。
- coordinator DECISION_SUCCESS_PROMPT：skill 返回的内容**本身就是对用户的提问**
  时，直接把提问转达给用户，不要调用 run_skills，等用户回答后再继续。
- coordinator 代码无特殊分支（回滚了早期的终止分支/结构化标记方案）。

本测试验证：
① SKILL_ASK_USER_FALLBACK_PROMPT 内容：指导文本提问、不含 JSON 结构化标记
② DECISION_SUCCESS_PROMPT 内容：SKILL 返回提问 → 直接转达，不调 run_skills
③ skill_executor 系统提示词确实拼接了 fallback 提示
④ coordinator 正常链路：SKILL 返回提问文本（无标记）→ LLM 按提示词直接回复用户
"""

from __future__ import annotations

import json
import sys
from contextlib import contextmanager
from pathlib import Path
from unittest.mock import patch

PROJECT_ROOT = Path(__file__).resolve().parents[2]
PKG_SRC = PROJECT_ROOT / "packages" / "agentflow-runtime" / "src"
if str(PKG_SRC) not in sys.path:
    sys.path.insert(0, str(PKG_SRC))
# prompts/__init__.py 使用顶层 `from prompts import ...`（entry 文件依赖），
# 需把 agentflow_runtime 目录本身加入 sys.path 才能直接导入 prompts.*。
_PKG_AGENT_RUNTIME = PKG_SRC / "agentflow_runtime"
if str(_PKG_AGENT_RUNTIME) not in sys.path:
    sys.path.insert(0, str(_PKG_AGENT_RUNTIME))


# ════════════════════════════════════════════════════════════════════
# ① / ② 提示词内容断言
# ════════════════════════════════════════════════════════════════════


def test_skill_fallback_prompt_guides_plain_text_questioning():
    """SKILL_ASK_USER_FALLBACK_PROMPT：指导文本提问，不含 JSON 结构化标记。

    通过 skill_executor 模块属性访问（其模块级 `from prompts import ...` 已成功），
    避免直接 import agentflow_runtime.prompts 触发 __init__ 的顶层导入问题。
    """
    from agentflow_runtime import skill_executor

    prompt = skill_executor.SKILL_ASK_USER_FALLBACK_PROMPT
    assert "ask_user" in prompt
    assert "不在工具列表中" in prompt
    assert "不要假装调用" in prompt
    assert "文本一次性询问" in prompt
    assert "直接作为 skill_result 的正文返回" in prompt
    # 已废弃的结构化标记不应存在
    assert "awaiting_user_input" not in prompt
    assert '"status"' not in prompt


def test_decision_success_prompt_guides_direct_reply():
    """DECISION_SUCCESS_PROMPT：SKILL 返回提问 → 直接转达，不调 run_skills。"""
    from agentflow_runtime import coordinator_decide

    prompt = coordinator_decide.DECISION_SUCCESS_PROMPT
    assert "本身就是对用户的提问" in prompt
    assert "直接把这个提问转达给用户" in prompt
    assert "不要调用 run_skills" in prompt


def test_fallback_prompt_exported_from_package():
    """新常量已从 prompts 包导出（skill_executor import 可用）。"""
    from agentflow_runtime import skill_executor
    from agentflow_runtime.prompts.skill import (
        SKILL_ASK_USER_FALLBACK_PROMPT as _direct,
    )

    assert skill_executor.SKILL_ASK_USER_FALLBACK_PROMPT == _direct


# ════════════════════════════════════════════════════════════════════
# ③ skill_executor 系统提示词拼接
# ════════════════════════════════════════════════════════════════════


class _FakeBus:
    def is_connected(self):
        return True

    def get(self, key):
        return None

    def get_global(self, key):
        if key.startswith("dispatch:"):
            return json.dumps({
                "skill_name": "outfit-stylist",
                "prompt": "请推荐穿搭",
                "load_skills": [],
            }, ensure_ascii=False)
        return None

    def put(self, key, value):
        return True

    def put_global(self, key, value):
        return True

    def delete(self, key):
        return True

    def response_push(self, *a, **kw):
        return True


def test_skill_executor_system_prompt_includes_fallback(monkeypatch, capsys):
    """skill_executor 构建 system_prompt 时拼接了 SKILL_ASK_USER_FALLBACK_PROMPT。"""
    from agentflow_runtime import skill_executor
    from agentflow_runtime.prompts.skill import SKILL_ASK_USER_FALLBACK_PROMPT

    monkeypatch.setenv("AF_NODE_ID", "skill_executor")
    monkeypatch.setenv("AF_DIALOGUE_ID", "d_fb")
    monkeypatch.setenv("AF_SESSION_ID", "s_fb")
    monkeypatch.setenv("AF_INPUT", "请推荐穿搭")
    monkeypatch.setenv("AF_EXEC_SKILL_KEY", "dispatch:1")
    monkeypatch.setenv("AF_DATABUS_UDS_FILE", "")
    monkeypatch.setenv("AF_SKIP_DOTENV", "1")
    monkeypatch.setenv("AGENT_PACKAGE_DIR", str(PROJECT_ROOT / "examples" / "agent" / "outfit-assistant"))

    fake_bus = _FakeBus()
    with patch.object(skill_executor, "DataBusClient", return_value=fake_bus), \
         patch.object(skill_executor, "call_llm") as mock_llm, \
         patch.object(skill_executor, "emit_event"), \
         patch.object(skill_executor, "AgentConfig") as mock_cfg, \
         patch.object(skill_executor, "_LocalToolRegistry") as mock_reg, \
         patch.object(skill_executor, "_ensure_artifacts_in_package",
                      side_effect=lambda artifacts: artifacts), \
         patch.object(skill_executor, "resolve_llm_config",
                      return_value={"model": "t", "api_key": "k", "base_url": "http://127.0.0.1:1/v1"}), \
         patch.object(skill_executor, "load_skills_from_list", return_value={}):
        mock_cfg.skill_max_iterations.return_value = 3
        mock_cfg.node_execution_timeout.return_value = 3600
        mock_cfg.skill_max_completion_tokens.return_value = 4000
        mock_cfg.skill_llm_temperature.return_value = 0.7
        mock_cfg.skill_llm_top_p.return_value = 1.0
        mock_cfg.skill_llm_frequency_penalty.return_value = 0.0
        mock_cfg.skill_llm_presence_penalty.return_value = 0.0
        mock_cfg.skill_llm_top_k.return_value = 0
        mock_cfg.skill_llm_stop.return_value = None
        mock_cfg.skill_llm_recovery_attempts.return_value = 2
        mock_reg.return_value.get_tools.return_value = []
        mock_reg.return_value.execute.side_effect = lambda n, a: "{}"

        # 捕获 call_llm 收到的 system prompt，断言包含 fallback 提示
        captured = {}

        def _capture_llm(messages, **kw):
            captured["system"] = messages[0]["content"]
            return {"choices": [{"message": {"content": "完成", "finish_reason": "stop"}}],
                    "usage": {"prompt_tokens": 1, "completion_tokens": 1}}

        mock_llm.side_effect = _capture_llm

        import io
        with patch("sys.stdout", io.StringIO()):
            skill_executor.execute()

    assert SKILL_ASK_USER_FALLBACK_PROMPT in captured.get("system", ""), (
        "skill_executor system_prompt 应包含 SKILL_ASK_USER_FALLBACK_PROMPT"
    )


# ════════════════════════════════════════════════════════════════════
# ④ coordinator 正常链路：SKILL 返回提问文本（无标记）→ LLM 直接回复
# ════════════════════════════════════════════════════════════════════


class _CoordFakeBus:
    def __init__(self):
        self.connected = True
        self.put_calls = []

    def is_connected(self):
        return True

    def get(self, key):
        return None

    def get_global(self, key):
        return None

    def put(self, key, value):
        self.put_calls.append((key, value))
        return True

    def put_global(self, key, value):
        self.put_calls.append((key, value))
        return True

    def delete(self, key):
        return True

    def response_push(self, *a, **kw):
        return True


@contextmanager
def _mock_coordinator(fake_bus, mock_llm):
    from agentflow_runtime import coordinator_decide as cd

    with patch.object(cd, "DataBusClient", return_value=fake_bus), \
         patch.object(cd, "call_llm", side_effect=mock_llm) as m_llm, \
         patch.object(cd, "emit_event"), \
         patch.object(cd, "AgentConfig") as mock_cfg, \
         patch.object(cd, "resolve_llm_config",
                      return_value={"model": "t", "api_key": "k", "base_url": "http://127.0.0.1:1/v1"}), \
         patch.object(cd, "get_recorder") as mock_rec:
        mock_cfg.coordinator_max_iterations.return_value = 5
        mock_cfg.max_completion_tokens.return_value = 4000
        mock_cfg.node_timeout_sec.return_value = 3600
        mock_cfg.llm_temperature.return_value = 0.7
        mock_cfg.llm_top_p.return_value = 1.0
        mock_cfg.fold_compression_enabled.return_value = False
        mock_cfg.protect_last_rounds.return_value = 0
        mock_cfg.skill_max_retries.return_value = 0
        mock_cfg.fold_dir.return_value = ""
        mock_cfg.hard_compression_enabled.return_value = False
        mock_cfg.hard_compression_model.return_value = ""
        mock_rec.return_value.is_connected = False
        yield cd, m_llm


def test_coordinator_normal_flow_with_question_result(monkeypatch, capsys):
    """coordinator 正常链路：SKILL 返回提问文本（无结构化标记）→ LLM 按提示词直接回复。

    验证回滚后 coordinator 无特殊分支也能正常完成——LLM 决策依据
    DECISION_SUCCESS_PROMPT（已强化"SKILL 返回提问→直接转达"）返回最终回答，
    不走 run_skills、不落兜底错误。
    """
    skill_ask = json.dumps({
        "skill_result": "请告诉我以下信息（越详细推荐越精准）：\n① 性别方向\n② 体型轮廓",
        "status": "completed", "skill_name": "outfit-stylist", "rounds_used": 3,
    }, ensure_ascii=False)

    monkeypatch.setenv("AF_NODE_ID", "coordinator_decide")
    monkeypatch.setenv("AF_DIALOGUE_ID", "d_verify_flow")
    monkeypatch.setenv("AF_SESSION_ID", "s_verify")
    monkeypatch.setenv("AF_DATABUS_UDS_FILE", "")
    monkeypatch.setenv("AF_SKIP_DOTENV", "1")
    monkeypatch.setenv("AF_LLM_API_KEY", "test-key-not-used")
    monkeypatch.setenv("AF_LLM_BASE_URL", "http://127.0.0.1:1/v1")
    monkeypatch.setenv(
        "AF_INPUT",
        json.dumps({"stream": False, "messages": [{"role": "user", "content": "帮我推荐一套穿搭"}]}),
    )
    monkeypatch.setenv("AF_COMPLETED_SKILL_NAME", "outfit-stylist")
    monkeypatch.setenv("AF_SKILL_SUCCESS", "true")
    monkeypatch.setenv("AF_SKILL_RESULT", skill_ask)
    monkeypatch.setenv("AF_SKILL_ARTIFACTS", "[]")

    fake_bus = _CoordFakeBus()

    def _llm_replies_directly(*a, **kw):
        # LLM 依据 DECISION_SUCCESS_PROMPT：SKILL 返回提问 → 直接转达，不调 run_skills
        return {
            "choices": [{"message": {
                "content": "请告诉我以下信息：\n① 性别方向\n② 体型轮廓",
                "finish_reason": "stop",
            }}],
            "usage": {"prompt_tokens": 10, "completion_tokens": 5},
        }

    with _mock_coordinator(fake_bus, _llm_replies_directly) as (cd, m_llm):
        cd.execute()

    out = capsys.readouterr().out
    last = json.loads(out.strip().splitlines()[-1])
    assert "请告诉我以下信息" in last["response"], f"LLM 应直接转达提问: {last}"
    assert "工具调用失败" not in last["response"], "不应落兜底错误"
    m_llm.assert_called_once()
    assert m_llm.call_args.kwargs["source"] == "coordinator"
    assert m_llm.call_args.kwargs["round_i"] == 1
    llm_messages = m_llm.call_args.args[0]
    completion_messages = [
        message for message in llm_messages
        if "Skill 'outfit-stylist' 已完成" in message.get("content", "")
    ]
    assert [message["role"] for message in completion_messages] == ["tool"]
    completion_tool = completion_messages[0]
    completion_index = llm_messages.index(completion_tool)
    reconstructed_call = llm_messages[completion_index - 1]["tool_calls"][0]
    assert completion_tool["tool_call_id"] == reconstructed_call["id"]
