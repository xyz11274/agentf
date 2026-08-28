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

import pytest

PROJECT_ROOT = Path(__file__).resolve().parents[2]
PKG_SRC = PROJECT_ROOT / "packages" / "agentflow-runtime" / "src"
if str(PKG_SRC) not in sys.path:
    sys.path.insert(0, str(PKG_SRC))


class _FakeDataBus:
    """模拟 DataBusClient，记录所有 put/put_global 调用供断言。"""

    def __init__(self, fail_put=None, fail_put_global=None):
        self.sys_puts: dict[str, str] = {}
        self.global_puts: dict[str, str] = {}
        self.fail_put = fail_put
        self.fail_put_global = fail_put_global

    def put(self, key, value):
        # 与 DataBusClient.put 同语义：写 sys namespace（dialogue 前缀）
        if self.fail_put and self.fail_put(key):
            return False
        self.sys_puts[key] = value if isinstance(value, str) else str(value)
        return True

    def put_global(self, key, value):
        if self.fail_put_global and self.fail_put_global(key):
            return False
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


def _call_handle(args: dict, dialogue_id: str = "dlg_test",
                 fake_bus: _FakeDataBus | None = None) -> tuple[str, _FakeDataBus]:
    """直接调 handle_run_skills_call，返回 (返回值文本, fake_bus)。"""
    from agentflow_runtime import coordinator_decide as cd

    if fake_bus is None:
        fake_bus = _FakeDataBus()
    ret = cd.handle_run_skills_call(args, fake_bus, "sess_test", dialogue_id)
    return ret, fake_bus


@pytest.mark.parametrize("raw", [
    {
        "choices": [{
            "message": {"role": "assistant", "content": ""},
            "finish_reason": "stop",
        }],
        "usage": {
            "prompt_tokens": 100,
            "completion_tokens": 10,
            "total_tokens": 110,
        },
    },
    {"choices": []},
    {"choices": [{}]},
])
def test_empty_llm_response_does_not_depend_on_zero_usage(raw):
    """已计费空答案和畸形 choice 都不能被 Coordinator 当作正常终态。"""
    from agentflow_runtime import coordinator_decide as cd

    assert cd.is_empty_llm_response(raw) is True


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
        assert manifest["request_index"] in {1, 2}
        assert manifest["force"] is False


def test_dispatch_batch_identity_shared_within_round():
    """同一 run_skills 调用共享 batch_id，每个执行仍由 exec_skill_key 唯一区分。"""
    args = {
        "skills": [
            {"name": "outfit-stylist", "prompt": "推荐穿搭"},
            {"name": "forge-image-gen", "prompt": "生成图"},
        ],
        "reason": "parallel batch",
    }
    _, fake_bus = _call_handle(args)

    next_skills = json.loads(fake_bus.sys_puts["next_skills"])
    batch_ids = {spec["batch_id"] for spec in next_skills}
    exec_keys = {spec["exec_skill_key"] for spec in next_skills}

    assert len(batch_ids) == 1
    batch_id = next(iter(batch_ids))
    assert batch_id.startswith("batch:dlg_test:")
    assert len(exec_keys) == len(next_skills)

    dispatch_manifests = [
        json.loads(value)
        for key, value in fake_bus.global_puts.items()
        if key.startswith("dispatch:dlg_test:")
    ]
    assert {manifest["batch_id"] for manifest in dispatch_manifests} == {batch_id}


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
    assert ret["dispatches"] == [{
        "request_index": 1,
        "skill_name": "outfit-stylist",
        "prompt": "推荐",
        "load_skills": [],
        "force": False,
        "exec_skill_key": ret["exec_skill_keys"][0],
        "batch_id": ret["batch_id"],
    }]


# ════════════════════════════════════════════════════════════════════
# ⑤ 单个 manifest 写失败不应连带终止同批其他 skill
# ════════════════════════════════════════════════════════════════════


def test_one_manifest_failure_keeps_other_skills_dispatchable():
    """三项中的第二项写失败时，只拒绝第二项并提交其余两项。"""
    fake_bus = _FakeDataBus(
        fail_put_global=lambda key: (
            key.startswith("dispatch:dlg_test:") and key.endswith(":2")
        ),
    )
    args = {
        "skills": [
            {"name": "skill-a", "prompt": "A"},
            {"name": "skill-b", "prompt": "B"},
            {"name": "skill-c", "prompt": "C"},
        ],
        "reason": "partial failure",
    }
    ret_text, fake_bus = _call_handle(args, fake_bus=fake_bus)

    ret = json.loads(ret_text)
    assert ret["status"] == "partially_dispatched"
    assert ret["skills"] == ["skill-a", "skill-c"]
    assert [item["request_index"] for item in ret["dispatches"]] == [1, 3]
    assert [item["skill_name"] for item in ret["dispatches"]] == [
        "skill-a", "skill-c",
    ]
    assert ret["rejected"] == [{
        "name": "skill-b",
        "request_index": 2,
        "stage": "manifest_write",
        "error": "DataBus put_global returned false",
    }]

    next_skills = json.loads(fake_bus.sys_puts["next_skills"])
    assert [spec["exec_skill_key"].rsplit(":", 1)[-1] for spec in next_skills] == [
        "1", "3",
    ]
    assert fake_bus.global_puts["last_prompt:dlg_test:skill-a"] == "A"
    assert "last_prompt:dlg_test:skill-b" not in fake_bus.global_puts
    assert fake_bus.global_puts["last_prompt:dlg_test:skill-c"] == "C"


def test_all_manifest_failures_do_not_publish_next_skills():
    """所有 manifest 都失败时，不发布空调度并让 Coordinator 重决策。"""
    fake_bus = _FakeDataBus(
        fail_put_global=lambda key: key.startswith("dispatch:dlg_test:"),
    )
    args = {
        "skills": [
            {"name": "skill-a", "prompt": "A"},
            {"name": "skill-b", "prompt": "B"},
        ],
        "reason": "total failure",
    }
    ret_text, fake_bus = _call_handle(args, fake_bus=fake_bus)

    ret = json.loads(ret_text)
    assert ret["status"] == "dispatch_failed"
    assert ret["skills"] == []
    assert len(ret["rejected"]) == 2
    assert {item["stage"] for item in ret["rejected"]} == {"manifest_write"}
    assert "next_skills" not in fake_bus.sys_puts
    assert not any(
        key.startswith("last_prompt:") for key in fake_bus.global_puts
    )


def test_next_skills_failure_does_not_report_dispatch_success():
    """manifest 成功但提交 next_skills 失败时，不能谎报已经调度。"""
    fake_bus = _FakeDataBus(fail_put=lambda key: key == "next_skills")
    args = {
        "skills": [
            {"name": "skill-a", "prompt": "A"},
            {"name": "skill-b", "prompt": "B"},
        ],
        "reason": "commit failure",
    }
    ret_text, fake_bus = _call_handle(args, fake_bus=fake_bus)

    ret = json.loads(ret_text)
    assert ret["status"] == "dispatch_failed"
    assert ret["skills"] == []
    assert len(ret["rejected"]) == 2
    assert {item["stage"] for item in ret["rejected"]} == {"next_skills_write"}
    assert "next_skills" not in fake_bus.sys_puts
    assert not any(
        key.startswith("last_prompt:") for key in fake_bus.global_puts
    )


# ════════════════════════════════════════════════════════════════════
# ⑥ dispatch 时记录 last_prompt(供 F2 双维度重复判定对比)
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


def test_completed_batch_builds_one_ordered_coordinator_message():
    """ALL join 的多个结果应合成一条消息，并保持原 dispatch 顺序。"""
    from agentflow_runtime import coordinator_decide as cd

    raw_batch = json.dumps({
        "batch_id": "batch:dlg_test:round",
        "results": [
            {
                "exec_skill_key": "dispatch:dlg_test:round:1",
                "framework_skill_name": "skill-runner",
                "business_skill_name": "outfit-stylist",
                "workflow_execution_id": "workflow-1",
                "workflow_success": True,
                "skill_success": True,
                "skill_status": "completed",
                "skill_result": '{"outfit":"blue"}',
                "artifacts_json": '[{"id":"artifact-1"}]',
            },
            {
                "exec_skill_key": "dispatch:dlg_test:round:2",
                "framework_skill_name": "skill-runner",
                "business_skill_name": "forge-image-gen",
                "workflow_execution_id": "workflow-2",
                "workflow_success": True,
                "skill_success": False,
                "skill_status": "tool_call_error",
                "skill_result": "image service unavailable",
                "artifacts_json": '[{"id":"artifact-2"}]',
            },
        ],
    }, ensure_ascii=False)

    batch = cd.parse_completed_batch(raw_batch, "completion_batch:test")
    message = cd.build_skill_batch_completion_message(batch)

    assert message.count("Skill 'batch ") == 1
    assert "success=partial" in message
    assert message.index("outfit-stylist") < message.index("forge-image-gen")
    assert '"outfit": "blue"' in message
    assert "image service unavailable" in message
    assert "部分成功、部分失败" in message
    assert "不得因为其他分支失败而重跑已成功的 skill" in message
    assert json.loads(cd.collect_batch_artifacts(batch)) == [
        {"id": "artifact-1"},
        {"id": "artifact-2"},
    ]

def test_skill_completion_replaces_matching_dispatch_ack_with_tool_result():
    """异步完成应回填原 tool_call，而不是伪造一轮 user 输入。"""
    from agentflow_runtime import coordinator_decide as cd

    batch_id = "batch:dlg_test:round"
    history = [
        {
            "role": "assistant",
            "content": "",
            "tool_calls": [{
                "id": "call-run-skills",
                "type": "function",
                "function": {"name": "run_skills", "arguments": "{}"},
            }],
        },
        {
            "role": "tool",
            "tool_call_id": "call-run-skills",
            "content": json.dumps({
                "status": "partially_dispatched",
                "batch_id": batch_id,
                "skills": ["skill-a"],
                "rejected": [{"name": "skill-b", "stage": "manifest_write"}],
            }),
        },
    ]

    result = cd.attach_skill_completion_result(
        history,
        "Skill batch completed",
        batch_id=batch_id,
        skill_names=["skill-a"],
    )

    assert result is history
    assert len(result) == 2
    assert result[1]["role"] == "tool"
    assert result[1]["tool_call_id"] == "call-run-skills"
    payload = json.loads(result[1]["content"])
    assert payload["status"] == "execution_completed"
    assert payload["batch_id"] == batch_id
    assert payload["dispatch"]["status"] == "partially_dispatched"
    assert payload["dispatch"]["rejected"][0]["name"] == "skill-b"
    assert payload["result"] == "Skill batch completed"
    assert not any(message.get("role") == "user" for message in result)


def test_skill_completion_reconstructs_valid_tool_pair_when_history_is_missing():
    """历史读取失败时也不能发送孤立 tool 消息或把结果提升为 user。"""
    from agentflow_runtime import coordinator_decide as cd

    result = cd.attach_skill_completion_result(
        [], "Skill result", skill_names=["outfit-stylist"]
    )

    assert [message["role"] for message in result] == ["assistant", "tool"]
    tool_call = result[0]["tool_calls"][0]
    assert tool_call["function"]["name"] == "run_skills"
    assert result[1]["tool_call_id"] == tool_call["id"]
    assert json.loads(result[1]["content"])["result"] == "Skill result"


def test_dialogue_artifacts_accumulate_across_batches():
    """后续空产物批次不能覆盖较早批次已经生成的附件。"""
    from agentflow_runtime import coordinator_decide as cd

    fake_bus = _FakeDataBus()
    first = cd.merge_dialogue_artifacts(
        fake_bus, '["outputs/outfit.png", "outputs/report.html"]'
    )
    second = cd.merge_dialogue_artifacts(
        fake_bus, '["outputs/report.html", "outputs/details.pdf"]'
    )

    assert json.loads(first) == [
        "outputs/outfit.png",
        "outputs/report.html",
    ]
    assert json.loads(second) == [
        "outputs/outfit.png",
        "outputs/report.html",
        "outputs/details.pdf",
    ]
    assert json.loads(fake_bus.sys_puts["_coordinator_artifacts"]) == json.loads(second)


@pytest.mark.parametrize("raw_batch", [
    "not-json",
    json.dumps({"batch_id": "batch:test", "results": []}),
    json.dumps({"batch_id": "batch:test", "results": [{}]}),
])
def test_completed_batch_rejects_missing_or_empty_terminal_results(raw_batch):
    """缺失终态不能被误判为一个成功的空 Batch。"""
    from agentflow_runtime import coordinator_decide as cd

    with pytest.raises(ValueError):
        cd.parse_completed_batch(raw_batch, "completion_batch:test")


def test_completed_batch_runtime_context_accessor(monkeypatch):
    """Worker 注入的 AF_COMPLETED_BATCH_KEY 应通过统一 accessor 读取。"""
    from agentflow_runtime.common import runtime_context

    monkeypatch.setenv("AF_COMPLETED_BATCH_KEY", "completion_batch:test")
    assert runtime_context.completed_batch_key() == "completion_batch:test"


def test_completed_batch_is_consumed_before_max_iterations_return(monkeypatch):
    """批次已成功解析时，max_iterations 提前返回不能把 final gate 卡死。"""
    from agentflow_runtime import coordinator_decide as cd

    completed_batch_key = "completion_batch:batch:dlg-max:round"
    consumed_attempts = 0

    def fail_first_two_consumed_writes(key):
        nonlocal consumed_attempts
        if key == f"{completed_batch_key}:consumed":
            consumed_attempts += 1
            return consumed_attempts < 3
        return False

    fake_bus = _FakeDataBus(fail_put=fail_first_two_consumed_writes)
    fake_bus.sys_puts["_coordinator_artifacts"] = json.dumps([
        "outputs/first.png",
    ])
    fake_bus.sys_puts[completed_batch_key] = json.dumps({
        "batch_id": "batch:dlg-max:round",
        "results": [{
            "exec_skill_key": "dispatch:dlg-max:round:1",
            "framework_skill_name": "skill-runner",
            "business_skill_name": "outfit-stylist",
            "workflow_success": True,
            "skill_success": True,
            "skill_status": "completed",
            "skill_result": "done",
            "artifacts_json": '["outputs/second.html"]',
        }],
    })
    fake_bus.sys_puts["_coordinator_round"] = "1"
    dialogue_end_payloads = []
    lifecycle_events = []
    batch_result_payloads = []
    fallback_artifacts = []

    def capture_hook(event_name, payload, **_kwargs):
        if event_name in {
            cd.hook_mounts.AGENT_DIALOGUE_START,
            cd.hook_events.COORDINATOR_TRIGGER,
        }:
            lifecycle_events.append(event_name)
        if event_name == cd.hook_events.COORDINATOR_BATCH_RESULT:
            batch_result_payloads.append(payload)
        if event_name == cd.hook_mounts.AGENT_DIALOGUE_END:
            dialogue_end_payloads.append(payload)
        return payload

    monkeypatch.setattr(cd, "DataBusClient", lambda: fake_bus)
    monkeypatch.setattr(cd, "_load_dotenv", lambda: None)
    monkeypatch.setattr(cd, "_init_skills", lambda: None)
    monkeypatch.setattr(cd, "bind_package_paths", lambda *args: None)
    monkeypatch.setattr(cd, "parse_agent_md", lambda path: ({}, ""))
    monkeypatch.setattr(cd, "resolve_llm_config", lambda *args, **kwargs: {
        "model": "test-model",
        "api_key": "test-key",
        "base_url": "http://unused.invalid",
    })
    monkeypatch.setattr(cd, "load_hooks_from_dir", lambda path: None)
    monkeypatch.setattr(cd, "hook_trigger", capture_hook)
    monkeypatch.setattr(cd, "emit_event", lambda *args, **kwargs: None)
    monkeypatch.setattr(cd, "put_e2e_response", lambda *args, **kwargs: None)

    def build_max_response(artifacts):
        fallback_artifacts.append(json.loads(artifacts))
        return "max rounds"

    monkeypatch.setattr(cd, "llm_error_response", build_max_response)
    recorder = MagicMock()
    recorder.is_connected = False
    monkeypatch.setattr(cd, "get_recorder", lambda: recorder)

    def unexpected_llm_call(*args, **kwargs):
        raise AssertionError("max_iterations path must return before LLM")

    monkeypatch.setattr(cd, "call_llm", unexpected_llm_call)
    monkeypatch.setenv("AF_INPUT", "original request")
    monkeypatch.setenv("AF_DIALOGUE_ID", "dlg-max")
    monkeypatch.setenv("AF_SESSION_ID", "session-max")
    monkeypatch.setenv("AF_COMPLETED_BATCH_KEY", completed_batch_key)
    monkeypatch.setenv("AF_MAX_COORDINATOR_ITERATIONS", "1")
    monkeypatch.delenv("AF_COMPLETED_SKILL_NAME", raising=False)

    cd.execute()

    assert fake_bus.sys_puts[f"{completed_batch_key}:consumed"] == "true"
    assert consumed_attempts == 3
    assert fallback_artifacts == [["outputs/first.png", "outputs/second.html"]]
    assert json.loads(fake_bus.sys_puts["_coordinator_artifacts"]) == fallback_artifacts[0]
    assert lifecycle_events == [cd.hook_events.COORDINATOR_TRIGGER]
    assert len(batch_result_payloads) == 1
    assert batch_result_payloads[0]["status"] == "true"
    assert batch_result_payloads[0]["results"][0]["skill_result"] == "done"
    assert len(dialogue_end_payloads) == 1
    assert dialogue_end_payloads[0]["session_id"] == "session-max"
    assert dialogue_end_payloads[0]["dialogue_id"] == "dlg-max"
    assert dialogue_end_payloads[0]["status"] == "max_iterations"
    assert dialogue_end_payloads[0]["response_text"] == "max rounds"
    assert "elapsed_s" in dialogue_end_payloads[0]


def test_dispatch_failure_reaches_after_dispatch_hook(monkeypatch):
    """manifest 全部失败时仍应触发分发后 Hook，供 sessionlog 结构化落盘。"""
    from agentflow_runtime import coordinator_decide as cd

    fake_bus = _FakeDataBus(
        fail_put_global=lambda key: key.startswith("dispatch:dlg-trace:"),
    )
    recorder = MagicMock()
    recorder.is_connected = False
    dispatch_payloads = []
    lifecycle_events = []
    retry_payloads = []

    def capture_hook(event_name, payload, **kwargs):
        if event_name in {
            cd.hook_mounts.AGENT_DIALOGUE_START,
            cd.hook_events.COORDINATOR_TRIGGER,
        }:
            lifecycle_events.append(event_name)
        if event_name == cd.hook_mounts.COORDINATOR_AFTER_SKILL_DISPATCH:
            dispatch_payloads.append(payload)
        if event_name == cd.hook_events.COORDINATOR_TOOL_LOOP_RETRY:
            retry_payloads.append(payload)
        return payload

    llm_responses = iter([
        {
            "choices": [{
                "message": {
                    "role": "assistant",
                    "content": "",
                    "tool_calls": [{
                        "id": "call-dispatch",
                        "type": "function",
                        "function": {
                            "name": "run_skills",
                            "arguments": json.dumps({
                                "skills": [{"name": "skill-a", "prompt": "A"}],
                                "reason": "trace failure",
                            }),
                        },
                    }],
                },
                "finish_reason": "tool_calls",
            }],
            "usage": {"prompt_tokens": 1, "completion_tokens": 1},
        },
        {
            "choices": [{
                "message": {
                    "role": "assistant",
                    "content": "分发失败已记录",
                },
                "finish_reason": "stop",
            }],
            "usage": {"prompt_tokens": 1, "completion_tokens": 1},
        },
    ])

    config = MagicMock()
    config.coordinator_max_iterations.return_value = 3
    config.max_completion_tokens.return_value = 4000
    config.fold_dir.return_value = ""
    config.protect_last_rounds.return_value = 0
    config.hard_compression_enabled.return_value = False
    config.hard_compression_model.return_value = ""
    config.node_timeout_sec.return_value = 3600
    config.llm_temperature.return_value = 0.7
    config.llm_top_p.return_value = 1.0
    config.llm_frequency_penalty.return_value = 0.0
    config.llm_presence_penalty.return_value = 0.0
    config.llm_top_k.return_value = 0
    config.llm_stop.return_value = None

    monkeypatch.setattr(cd, "DataBusClient", lambda: fake_bus)
    monkeypatch.setattr(cd, "AgentConfig", config)
    monkeypatch.setattr(cd, "_load_dotenv", lambda: None)
    monkeypatch.setattr(cd, "_init_skills", lambda: None)
    monkeypatch.setattr(cd, "_AVAILABLE_SKILLS", ["skill-a"])
    monkeypatch.setattr(cd, "bind_package_paths", lambda *args: None)
    monkeypatch.setattr(cd, "parse_agent_md", lambda path: ({}, ""))
    monkeypatch.setattr(cd, "resolve_llm_config", lambda *args, **kwargs: {
        "model": "test-model", "api_key": "test-key", "base_url": "http://unused.invalid",
    })
    monkeypatch.setattr(cd, "load_hooks_from_dir", lambda path: None)
    monkeypatch.setattr(cd, "hook_trigger", capture_hook)
    monkeypatch.setattr(cd, "emit_event", lambda *args, **kwargs: None)
    monkeypatch.setattr(cd, "put_e2e_response", lambda *args, **kwargs: None)
    monkeypatch.setattr(cd, "get_recorder", lambda: recorder)
    monkeypatch.setattr(cd, "call_llm", lambda *args, **kwargs: next(llm_responses))
    monkeypatch.setattr(cd, "compress_messages", lambda messages, *args, **kwargs: (messages, "skip"))
    monkeypatch.setattr(cd, "build_round_reminder", lambda *args, **kwargs: None)
    monkeypatch.setenv("AF_INPUT", "请执行 skill-a")
    monkeypatch.setenv("AF_DIALOGUE_ID", "dlg-trace")
    monkeypatch.setenv("AF_SESSION_ID", "session-trace")
    monkeypatch.delenv("AF_COMPLETED_SKILL_NAME", raising=False)
    monkeypatch.delenv("AF_COMPLETED_BATCH_KEY", raising=False)

    cd.execute()

    assert lifecycle_events == [
        cd.hook_mounts.AGENT_DIALOGUE_START,
        cd.hook_events.COORDINATOR_TRIGGER,
    ]
    assert len(dispatch_payloads) == 1
    assert dispatch_payloads[0]["status"] == "dispatch_failed"
    assert dispatch_payloads[0]["requested_skill_names"] == ["skill-a"]
    assert dispatch_payloads[0]["skill_names"] == []
    assert dispatch_payloads[0]["rejected"][0]["stage"] == "manifest_write"
    assert len(retry_payloads) == 1
    assert retry_payloads[0]["action"] == "retry"
    assert retry_payloads[0]["reason"] == "no_skill_dispatched"
    assert retry_payloads[0]["round"] == 1
    assert retry_payloads[0]["tool_loop_round"] == 1
    assert retry_payloads[0]["tool_calls"][0]["function"]["name"] == "run_skills"

@pytest.mark.parametrize(
    ("args", "error_fragment"),
    [
        ({}, "skills must be a non-empty array"),
        ({"skills": []}, "skills must be a non-empty array"),
        ({"skills": "outfit-stylist"}, "skills must be a non-empty array"),
        ({"skills": ["outfit-stylist"]}, "skills[0] must be an object"),
        ({"skills": [{"prompt": "推荐穿搭"}]}, "skills[0].name"),
        ({"skills": [{"name": "outfit-stylist", "prompt": ""}]}, "skills[0].prompt"),
        ({
            "skills": [{
                "name": "outfit-stylist",
                "prompt": "推荐穿搭",
                "load_skills": "outfit-stylist",
            }],
        }, "skills[0].load_skills"),
    ],
)
def test_invalid_run_skills_arguments_do_not_write_databus(args, error_fragment):
    """Malformed semantic arguments become a Tool error before any dispatch write."""
    result_text, fake_bus = _call_handle(args)
    result = json.loads(result_text)

    assert result["status"] == "invalid_arguments"
    assert error_fragment in result["error"]
    assert result["skills"] == []
    assert result["rejected"][0]["stage"] == "argument_validation"
    assert fake_bus.sys_puts == {}
    assert fake_bus.global_puts == {}


def test_run_skills_validator_enforces_available_skill_names():
    """A schema-valid but unavailable Skill must enter correction, not dispatch."""
    from agentflow_runtime import coordinator_decide as cd

    args = {
        "skills": [{"name": "unknown-skill", "prompt": "执行任务"}],
        "reason": "test",
    }

    error = cd._validate_run_skills_args(args, ["outfit-stylist"])

    assert "unknown-skill" in error
    assert "outfit-stylist" in error
    assert cd._validate_run_skills_args(
        {"skills": [{"name": "outfit-stylist", "prompt": "执行任务"}]},
        ["outfit-stylist"],
    ) == ""


def test_main_emits_dialogue_error_and_reraises(monkeypatch):
    """Production entrypoint records an unexpected terminal without swallowing it."""
    from agentflow_runtime import coordinator_decide as cd

    hook_calls = []

    def fail_execute():
        raise RuntimeError("coordinator failure")

    def record_hook(name, payload, **kwargs):
        hook_calls.append((name, payload, kwargs))
        return payload

    monkeypatch.setattr(cd, "execute", fail_execute)
    monkeypatch.setattr(cd, "load_hooks_from_dir", lambda _path: None)
    monkeypatch.setattr(cd, "hook_trigger", record_hook)
    monkeypatch.setenv("AF_DIALOGUE_ID", "dlg-error")
    monkeypatch.setenv("AF_SESSION_ID", "session-error")

    with pytest.raises(RuntimeError, match="coordinator failure"):
        cd.main()

    assert len(hook_calls) == 1
    name, payload, kwargs = hook_calls[0]
    assert name == cd.hook_events.AGENT_DIALOGUE_ERROR
    assert payload["dialogue_id"] == "dlg-error"
    assert payload["session_id"] == "session-error"
    assert payload["status"] == "unhandled_exception"
    assert payload["error_type"] == "RuntimeError"
    assert payload["error"] == "coordinator failure"
    assert "elapsed_s" in payload
    assert isinstance(kwargs["ctx"], cd.AgentContext)
