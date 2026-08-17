"""回归测试：handle_run_skills_call 不应把 force=true 硬编码成默认值。

死链根因（2026-08-11 实测日志 agentflowd.log 21:30:39 → 21:31:00）：
  ① Python 侧 handle_run_skills_call 把每个 next_skills spec 都硬编码
     `"force": True`（coordinator_decide.py:235）。
  ② C++ 侧 agent.cpp:2904 的 force 判断只看 `spec.name == "skill-runner"` 是否
     在 launched_skills 里——所有走 skill-runner 的不同 skill（outfit-stylist /
     forge-image-gen）都被当成同一个"已 launch" skill。
  ③ 第二轮 run_skills 调度 outfit-stylist 时，C++ 看到 next_skills 里有
     force=true 的 skill-runner 且 skill-runner 已在 launched_skills → 触发
     force re-launch：删 result/confidence、reset skills_scheduled、用同一
     task_id 再 launch 一次。
  ④ UI 已收到 _final 帧（主消息渲染完），又来一轮 llm_chunk + skill 卡片 →
     前端 store 状态被污染 → 渲染崩。

设计意图比对（F5/M9）：
  - builtin_dispatcher.h:28 force 是显式预留的 re-launch 能力，
    "Force re-launch: clear previous result and re-schedule"。
  - prompts/coordinator.py:29 给 coordinator 的约束：
    "不得重复 dispatch 已完成的 skill……仅当显式传 force=true 时才允许重新执行"。
  ⇒ force=true 是 coordinator 在特殊场景下"主动决定重排某 skill"的逃生口，
     不是每次 run_skills 都该带的默认值。

本测试验证：
  ① 默认 run_skills（未显式传 force）→ next_skills 中 force 应为 false 或缺失，
     不能硬编码为 true。
  ② coordinator 显式传 force=true（逃生口）→ next_skills 中 force=true 才允许。
  ③ 多 skill 同批调度 → 每个 spec 的 force 应独立反映调用方意图，不全部硬编码。
  ④ dispatch manifest（dispatch_key）应正确写入每个 skill 的 exec_skill_key。
"""

from __future__ import annotations

import json
import sys
from pathlib import Path
from unittest.mock import MagicMock

PROJECT_ROOT = Path(__file__).resolve().parents[2]
PKG_SRC = PROJECT_ROOT / "packages" / "agentflow-runtime" / "src"
if str(PKG_SRC) not in sys.path:
    sys.path.insert(0, str(PKG_SRC))


class _FakeDataBus:
    """模拟 DataBusClient，记录所有 put/put_global 调用供断言。"""

    def __init__(self):
        self.sys_puts: dict[str, str] = {}
        self.global_puts: dict[str, str] = {}

    def put(self, key, value):
        # 与 DataBusClient.put 同语义：写 sys namespace（dialogue 前缀）
        self.sys_puts[key] = value if isinstance(value, str) else str(value)
        return True

    def put_global(self, key, value):
        self.global_puts[key] = value if isinstance(value, str) else str(value)
        return True

    def get(self, key):
        return self.sys_puts.get(key)

    def get_global(self, key):
        return self.global_puts.get(key)

    def delete(self, key):
        self.sys_puts.pop(key, None)
        return True

    def is_connected(self):
        return True


def _call_handle(args: dict, dialogue_id: str = "dlg_test") -> tuple[str, _FakeDataBus]:
    """直接调 handle_run_skills_call，返回 (返回值文本, fake_bus)。"""
    from agentflow_runtime import coordinator_decide as cd

    fake_bus = _FakeDataBus()
    ret = cd.handle_run_skills_call(args, fake_bus, "sess_test", dialogue_id)
    return ret, fake_bus


# ════════════════════════════════════════════════════════════════════
# ① 默认 run_skills 不应硬编码 force=true（根因 bug 复现）
# ════════════════════════════════════════════════════════════════════


def test_default_run_skills_does_not_force_relaunch():
    """未显式传 force 的普通 run_skills → next_skills 中 force 应为 false 或缺失。

    死链根因：coordinator_decide.py:235 硬编码 `"force": True`，
    导致 framework 每轮都走 force re-launch 路径。
    """
    args = {
        "skills": [
            {"name": "outfit-stylist", "prompt": "推荐穿搭"},
            {"name": "forge-image-gen", "prompt": "生成效果图"},
        ],
        "reason": "用户首次提问",
    }
    ret_text, fake_bus = _call_handle(args)

    # next_skills 必须被写入
    assert "next_skills" in fake_bus.sys_puts, "handle_run_skills_call 必须写 next_skills"

    next_skills = json.loads(fake_bus.sys_puts["next_skills"])
    assert isinstance(next_skills, list) and len(next_skills) == 2

    for spec in next_skills:
        # 根因 bug：force 硬编码为 True。修复后应为 False 或字段缺失。
        assert spec.get("force", False) is False, (
            "默认 run_skills 不应带 force=true——这是预留的 re-launch 逃生口，"
            "硬编码成默认值会导致同一 dialogue 内多次 run_skills 触发 framework "
            "误判 force re-launch（实测 2026-08-11 死链）。"
        )
        # 但 name 和 exec_skill_key 必须保留（这是 skill 调度的核心契约）
        assert spec["name"] == "skill-runner", "所有 skill 走 skill-runner task"
        assert spec["exec_skill_key"].startswith("dispatch:dlg_test:"), (
            "exec_skill_key 必须携带 dialogue_id 用于区分不同 skill 调度实例"
        )


# ════════════════════════════════════════════════════════════════════
# ② 显式传 force=true 的逃生口应被保留
# ════════════════════════════════════════════════════════════════════


def test_explicit_force_true_is_preserved():
    """coordinator 显式传 force=true（逃生口）→ next_skills 中 force=true 才允许。

    设计意图（builtin_dispatcher.h:28 / prompts/coordinator.py:29）：
    force=true 是 coordinator "主动决定重排某 skill"的逃生口，
    修复不应删除这个能力，只应让它成为显式 opt-in 而非默认。
    """
    args = {
        "skills": [
            # 调用方显式要求重排这个 skill
            {"name": "forge-image-gen", "prompt": "用不同种子重新生成", "force": True},
            # 这个 skill 是首次调度，不应带 force
            {"name": "outfit-stylist", "prompt": "推荐穿搭"},
        ],
        "reason": "用户不满意效果图要求重生成",
    }
    ret_text, fake_bus = _call_handle(args)

    next_skills = json.loads(fake_bus.sys_puts["next_skills"])
    assert len(next_skills) == 2

    # 第一个 skill：显式 force=true → 应保留
    assert next_skills[0].get("force") is True, (
        "调用方显式传 force=true 的逃生口必须被保留——这是设计的预留能力，"
        "修复根因 bug 时不应破坏这个能力。"
    )
    # 第二个 skill：未传 force → 应为 false / 缺失
    assert next_skills[1].get("force", False) is False, (
        "未显式传 force 的 skill 不应被 force=true——逃生口是 opt-in 不是默认。"
    )


# ════════════════════════════════════════════════════════════════════
# ③ dispatch manifest 每个skill 应独立写 exec_skill_key（不变量）
# ════════════════════════════════════════════════════════════════════


def test_dispatch_manifest_per_skill_exec_key():
    """每个 skill 的 dispatch manifest 应独立写入，exec_skill_key 互不相同。

    不变量 I-5（单一真相来源）：每个 skill 调度实例的 exec_skill_key 必须唯一，
    这是 C++ 侧区分不同 skill �度的依据（agent.cpp force 判断应同时看这个字段）。
    """
    args = {
        "skills": [
            {"name": "outfit-stylist", "prompt": "推荐穿搭", "load_skills": ["outfit-stylist"]},
            {"name": "forge-image-gen", "prompt": "生成图", "load_skills": ["forge-image-gen"]},
        ],
        "reason": "",
    }
    ret_text, fake_bus = _call_handle(args)

    # dispatch manifest 应被写入 global（每个 skill 一条）
    dispatch_keys = [k for k in fake_bus.global_puts if k.startswith("dispatch:dlg_test:")]
    assert len(dispatch_keys) == 2, "每个 skill 应独立写一条 dispatch manifest"

    # exec_skill_key 应互不相同
    next_skills = json.loads(fake_bus.sys_puts["next_skills"])
    exec_keys = [s["exec_skill_key"] for s in next_skills]
    assert len(set(exec_keys)) == 2, "exec_skill_key 必须互不相同（区分不同 skill 调度实例）"

    # dispatch manifest 内容应正确
    for k in dispatch_keys:
        manifest = json.loads(fake_bus.global_puts[k])
        assert "skill_name" in manifest
        assert "prompt" in manifest
        assert "load_skills" in manifest
        assert isinstance(manifest["load_skills"], list)


# ════════════════════════════════════════════════════════════════════
# ④ 返回值契约不变（status=dispatched）
# ════════════════════════════════════════════════════════════════════


def test_return_value_contract_unchanged():
    """修复不应破坏 handle_run_skills_call 的返回值契约。"""
    args = {
        "skills": [{"name": "outfit-stylist", "prompt": "推荐"}],
        "reason": "test",
    }
    ret_text, _ = _call_handle(args)

    ret = json.loads(ret_text)
    assert ret["status"] == "dispatched"
    assert ret["skills"] == ["outfit-stylist"]
    assert "message" in ret
    assert ret["reason"] == "test"


# ════════════════════════════════════════════════════════════════════
# ⑤ dispatch 时记录 last_prompt(供 F2 双维度重复判定对比)
# ════════════════════════════════════════════════════════════════════


def test_dispatch_records_last_prompt():
    """每个 skill dispatch 时应写入 last_prompt:{dialogue_id}:{skill_name}。

    2026-08-13 双维度升级:coordinator re-trigger 时读取该 key 对比本次 prompt,
    同名 skill + 同 prompt 才算重复;同名不同 prompt(新任务)放行。
    """
    args = {
        "skills": [{"name": "outfit-stylist", "prompt": "推荐一套穿搭并返回HTML"}],
        "reason": "用户请求穿搭推荐",
    }
    _, fake_bus = _call_handle(args)

    key = "last_prompt:dlg_test:outfit-stylist"
    assert key in fake_bus.global_puts, (
        "dispatch 时必须记录 last_prompt key,否则 re-trigger 无法做双维度重复判定"
    )
    assert fake_bus.global_puts[key] == "推荐一套穿搭并返回HTML"


def test_dispatch_records_last_prompt_per_skill():
    """多 skill 同批 dispatch 时,每个 skill 独立记录自己的 last_prompt。"""
    args = {
        "skills": [
            {"name": "outfit-stylist", "prompt": "推荐穿搭"},
            {"name": "forge-image-gen", "prompt": "生成效果图"},
        ],
        "reason": "",
    }
    _, fake_bus = _call_handle(args)

    assert fake_bus.global_puts.get("last_prompt:dlg_test:outfit-stylist") == "推荐穿搭"
    assert fake_bus.global_puts.get("last_prompt:dlg_test:forge-image-gen") == "生成效果图"
