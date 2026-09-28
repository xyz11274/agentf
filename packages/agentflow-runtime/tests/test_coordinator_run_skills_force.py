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

    def get_strict(self, key):
        return self.get(key)

    def get_global(self, key):
        return self.global_puts.get(key)

    def delete(self, key):
        self.sys_puts.pop(key, None)
        return True

    def is_connected(self):
        return True


def test_evolution_orders_real_dispatch_manifests_and_merges_one_tool_result(monkeypatch, tmp_path):
    """包控制依赖；v1 继续使用原派发函数和异步批次协议。"""
    import os
    import subprocess
    from types import SimpleNamespace
    from agentflow_runtime import coordinator_decide as cd

    repo = Path(__file__).resolve().parents[3]
    build = repo / "build/trajectory-evolution"
    if not (build / "test_integration_service").is_file() or not (build / "libtrajectory_evolution_local.so").is_file():
        pytest.skip("requires the trajectory-evolution native test build")
    monkeypatch.syspath_prepend(str(repo / "tools/trajectory_evolution/clients/python"))
    from evolution_local import LocalEvolution
    monkeypatch.setenv("INTEGRATION_TEST_A", "fixture-principal-alpha-00000001")
    monkeypatch.setenv("INTEGRATION_TEST_B", "fixture-principal-bravo-00000002")
    subprocess.run([str(build / "test_integration_service"), "--fixture", str(tmp_path)], check=True, capture_output=True)
    path = tmp_path / "service.json"
    config = json.loads(path.read_text())
    config["auto_evaluate"] = False
    config["runtime_options"]["schedule_rules"] = [dict(rule_id="prepare-first",
        target_scope="agent:my-agent/actor:assistant", before_operation="prepare", after_operation="image",
        require_success=True, reason="prepare first")]
    path.write_text(json.dumps(config))
    client = LocalEvolution(build / "libtrajectory_evolution_local.so", path, os.environ["INTEGRATION_TEST_A"], background=False)
    facade = SimpleNamespace(point=lambda fields, **kwargs: client.point(fields))
    monkeypatch.setattr(cd, "_evolution", facade)
    bus, dialogue = _FakeDataBus(), "real-dispatch-contract"
    try:
        client.point(dict(phase="before", kind="run", ref=dict(run=dialogue), input="test", level="L2", capabilities=["schedule"]))
        specs = [dict(name="image", prompt="B"), dict(name="prepare", prompt="A"), dict(name="report", prompt="C")]
        binding = dict(items="/skills", operation="/name", receipt="/batch_id", accepted="/dispatches",
            accepted_index="/request_index", accepted_id="/exec_skill_key", results="/results",
            result_id="/exec_skill_key", result_success="/skill_success", result_operation="/business_skill_name",
            result_status="/skill_status", result_output="/skill_result")
        submit = lambda args: cd.handle_run_skills_call(args, bus, "session", dialogue)
        ack = json.loads(client.dispatch(dict(name="run_skills", ref=dict(run=dialogue, key="plan"),
            input=dict(skills=specs), binding=binding), submit, result_format="json_text"))

        def complete_wave():
            result = []
            for spec in json.loads(bus.sys_puts["next_skills"]):
                manifest = json.loads(bus.global_puts[spec["exec_skill_key"]])
                result.append(dict(exec_skill_key=spec["exec_skill_key"], business_skill_name=manifest["skill_name"],
                    skill_success=True, workflow_success=True, skill_status="success", skill_result="done"))
                client.point(dict(phase="after", kind="custom", ref=dict(run=dialogue, key=spec["exec_skill_key"]), output="done"))
            return dict(batch_id=manifest["batch_id"], results=result)

        first = complete_wave()
        assert [row["business_skill_name"] for row in first["results"]] == ["prepare", "report"]
        resumed = client.resume_dispatch(dict(ref=dict(run=dialogue, key="first"), input=first), submit, result_format="json_text")
        assert resumed["pending"]
        second = complete_wave()
        assert [row["business_skill_name"] for row in second["results"]] == ["image"]
        assert second["batch_id"] != first["batch_id"]
        resumed = client.resume_dispatch(dict(ref=dict(run=dialogue, key="second"), input=second), submit, result_format="json_text")
        assert not resumed["pending"]
        batch = resumed["completion"]
        assert batch["batch_id"] == ack["batch_id"]
        history = [{"role": "assistant", "tool_calls": [{"id": "call-1", "type": "function",
            "function": {"name": "run_skills", "arguments": json.dumps(dict(skills=specs))}}]},
            {"role": "tool", "tool_call_id": "call-1", "content": json.dumps(ack)}]
        history = cd.attach_skill_completion_result(history, cd.build_skill_batch_completion_message(batch), batch_id=batch["batch_id"])
        assert len(history) == 2 and history[1]["tool_call_id"] == "call-1"
        assert json.loads(history[1]["content"])["status"] == "execution_completed"
        assert client.point(dict(phase="after", kind="run", ref=dict(run=dialogue), output="done"))["capture_complete"]
    finally:
        client.close()


def _call_handle(args: dict, dialogue_id: str = "dlg_test",
                 fake_bus: _FakeDataBus | None = None) -> tuple[str, _FakeDataBus]:
    """直接调 handle_run_skills_call，返回 (返回值文本, fake_bus)。"""
    from agentflow_runtime import coordinator_decide as cd

    if fake_bus is None:
        fake_bus = _FakeDataBus()
    ret = cd.handle_run_skills_call(args, fake_bus, "sess_test", dialogue_id)
    return ret, fake_bus


def test_l2_guard_rejects_one_skill_before_manifest_without_blocking_sibling(monkeypatch):
    from agentflow_runtime import coordinator_decide as cd

    seen = []

    class Guard:
        def point(self, request, **_kwargs):
            seen.append(request)
            return {"decision": "reject", "reason": "two settled failures"} if (
                request.get("phase") == "guard" and request.get("name") == "forge-image-gen"
            ) else {"decision": "allow"}

    monkeypatch.setattr(cd, "_evolution", Guard())
    bus = _FakeDataBus()
    result = json.loads(cd.handle_run_skills_call({"skills": [
        {"name": "forge-image-gen", "prompt": "retry"},
        {"name": "outfit-stylist", "prompt": "independent"},
    ]}, bus, "session", "guard-dialogue", evolution_parent_ref="skill-plan:1:1"))
    assert [entry["name"] for entry in result["rejected"]] == ["forge-image-gen"]
    assert result["skills"] == ["outfit-stylist"]
    assert len([key for key in bus.global_puts if key.startswith("dispatch:")]) == 1
    assert [entry["name"] for entry in seen if entry.get("phase") == "guard"] == [
        "forge-image-gen", "outfit-stylist",
    ]


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


@pytest.mark.parametrize("recovers,max_rounds", [(True, 3), (False, 3), (False, 1)])
def test_incomplete_final_answer_is_recovered_once_or_failed(monkeypatch, recovers, max_rounds):
    from agentflow_runtime import coordinator_decide as cd

    fake_bus, recorder, config = _FakeDataBus(), MagicMock(), MagicMock()
    recorder.is_connected = False
    config.coordinator_max_iterations.return_value = max_rounds
    config.max_completion_tokens.return_value = 4000
    config.node_timeout_sec.return_value = 3600
    config.llm_top_k.return_value = 0
    config.llm_stop.return_value = None
    config.llm_temperature.return_value = 0.7
    config.llm_top_p.return_value = 1.0
    config.llm_frequency_penalty.return_value = 0
    config.llm_presence_penalty.return_value = 0
    raw = "<think>first</think>\n<think>unfinished\n</｜DSML｜tool_calls>"
    responses = [raw, "<think>one</think><think>two</think>完整答复" if recovers else raw]
    calls, events = [], []

    def call(messages, **kwargs):
        calls.append(json.loads(json.dumps(messages)))
        return {"choices": [{"finish_reason": "stop", "message": {
            "role": "assistant", "content": responses[len(calls)-1],
        }}], "usage": {"prompt_tokens": 2, "completion_tokens": 2}}

    def hook(name, payload, **kwargs):
        events.append((name, payload))
        return payload

    for name, replacement in {
        "DataBusClient": lambda: fake_bus, "AgentConfig": config, "get_recorder": lambda: recorder,
        "_load_dotenv": lambda: None, "_init_skills": lambda: None, "_AVAILABLE_SKILLS": ["skill-a"],
        "bind_package_paths": lambda *args: None, "parse_agent_md": lambda _path: ({}, ""),
        "resolve_llm_config": lambda *args, **kwargs: {"model":"test", "api_key":"test-key", "base_url":"http://unused.invalid"},
        "load_hooks_from_dir": lambda _path: None, "hook_trigger": hook, "emit_event": lambda *a, **k: None,
        "put_e2e_response": lambda *a, **k: None, "call_llm": call, "_evolution": None,
        "compress_messages": lambda m, *a, **k: (m, "skip"), "build_round_reminder": lambda *a, **k: None,
    }.items():
        monkeypatch.setattr(cd, name, replacement)
    monkeypatch.setenv("AF_INPUT", "完成任务")
    monkeypatch.setenv("AF_DIALOGUE_ID", "incomplete-final")
    monkeypatch.setenv("AF_SESSION_ID", "incomplete-final")
    monkeypatch.delenv("AF_COMPLETED_SKILL_NAME", raising=False)
    monkeypatch.delenv("AF_COMPLETED_BATCH_KEY", raising=False)
    cd.execute()
    assert len(calls) == min(2, max_rounds)
    if len(calls) == 2:
        assert calls[1][-1]["content"] == cd.LLM_FINAL_RESPONSE_RECOVERY_PROMPT
        assert raw not in json.dumps(calls[1])
    endings = [p for n,p in events if n == cd.hook_mounts.AGENT_DIALOGUE_END]
    assert len(endings) == 1
    assert endings[0]["status"] == ("normal" if recovers else "llm_error")
    assert "<think>" not in endings[0]["response_text"]
    assert "DSML" not in endings[0]["response_text"]
    if recovers:
        assert endings[0]["response_text"] == "完整答复"


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


@pytest.mark.parametrize(("skills", "status", "stage", "failures"), [
    ([{"name": "skill-a", "prompt": "A"}], "dispatch_failed", "manifest_write", 1),
    ([], "invalid_arguments", "argument_validation", 1),
    ([], "invalid_arguments", "argument_validation", 2),
])
def test_dispatch_failure_reaches_after_dispatch_hook(monkeypatch, skills, status, stage, failures):
    """连续校验拒绝必须逐次观测；有效参数分支沿用原包装，不重复打点或派发。"""
    from agentflow_runtime import coordinator_decide as cd

    fake_bus = _FakeDataBus(
        fail_put_global=lambda key: key.startswith("dispatch:dlg-trace:"),
    )
    recorder = MagicMock()
    recorder.is_connected = False
    dispatch_payloads = []
    lifecycle_events = []
    retry_payloads = []
    point_events = []
    action_points = []
    execution_order = []

    def capture_point(fields, **kwargs):
        if fields.get("kind") == "action":
            action_points.append(fields)
        if fields.get("name") == "tool.result":
            point_events.append(fields)
            execution_order.append("tool.result")
        # 与真实打点接口一致，未拦截的 guard 也返回允许决定。
        return {"input": fields.get("input"), "decision": "allow"}

    evolution = MagicMock()
    evolution.allow_loop.return_value = True
    evolution.point.side_effect = capture_point
    evolution.call.side_effect = lambda fields, execute, **kwargs: execute(fields["input"])
    evolution.dispatch.side_effect = lambda fields, execute, **kwargs: execute(fields["input"])

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
                                "skills": skills,
                                "reason": "trace failure",
                            }),
                        },
                    }],
                },
                "finish_reason": "tool_calls",
            }],
            "usage": {"prompt_tokens": 1, "completion_tokens": 1},
        },
    ] * failures + [
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
    config.coordinator_max_iterations.return_value = 4
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
    def next_llm_response(*args, **kwargs):
        execution_order.append("model")
        return next(llm_responses)

    monkeypatch.setattr(cd, "call_llm", next_llm_response)
    monkeypatch.setattr(cd, "_evolution", evolution)
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
    assert len(dispatch_payloads) == failures
    assert dispatch_payloads[0]["status"] == status
    assert dispatch_payloads[0]["requested_skill_names"] == [skill["name"] for skill in skills]
    assert dispatch_payloads[0]["skill_names"] == []
    assert dispatch_payloads[0]["rejected"][0]["stage"] == stage
    assert len(retry_payloads) == failures
    assert retry_payloads[0]["action"] == "retry"
    assert retry_payloads[0]["reason"] == "no_skill_dispatched"
    assert retry_payloads[0]["round"] == 1
    assert retry_payloads[0]["tool_loop_round"] == 1
    assert retry_payloads[0]["tool_calls"][0]["function"]["name"] == "run_skills"
    assert execution_order == ["model", "tool.result"] * failures + ["model"]
    assert len(point_events) == failures
    point = point_events[0]
    assert point["phase"] == "event"
    assert point["kind"] == "custom"
    assert point["ref"] == {"run": "dlg-trace", "key": "coordinator-tool:1:0"}
    assert point["input"]["tool_call_id"] == "call-dispatch"
    assert point["input"]["name"] == "run_skills"
    assert json.loads(point["input"]["arguments"])["skills"] == skills
    assert json.loads(point["input"]["output"])["status"] == status
    assert evolution.allow_loop.call_args_list[0].args[0]["actor_id"] == "coordinator"
    if status == "invalid_arguments":
        assert len(action_points) == 2 * failures
        for index in range(failures):
            before, after = action_points[2 * index:2 * index + 2]
            assert before["phase"] == "before" and before["apply_policy"] is False
            assert before["name"] == "run_skills" and before["actor_id"] == "coordinator"
            assert before["input"] == {"skills": skills, "reason": "trace failure"}
            assert before["ref"] == after["ref"] == {
                "run": "dlg-trace", "key": f"coordinator-dispatch:{index + 1}:0",
            }
            assert after["phase"] == "after" and after["status"] == "failed"
            assert after["executed"] is False and after["actual_input"] == before["input"]
            assert after["error"]["code"] == "invalid_arguments"
            assert after["error"]["retryability"] == "unknown"
        assert evolution.dispatch.call_count == 0
        assert not any(key.startswith("dispatch:") for key in fake_bus.global_puts)
    else:
        assert not action_points
        assert evolution.dispatch.call_count == 1

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


def test_run_skills_schema_matches_nonempty_dispatch_validation():
    """发给模型的列表约束应与执行侧的非空校验一致。"""
    from agentflow_runtime import coordinator_decide as cd
    from agentflow_runtime.prompts.coordinator import RUN_SKILLS_TOOL_DEFINITION

    schema = RUN_SKILLS_TOOL_DEFINITION["function"]["parameters"]["properties"]["skills"]
    assert schema["minItems"] == 1
    assert cd._validate_run_skills_args({"skills": []}, ["outfit-stylist"])
    assert cd._validate_run_skills_args({
        "skills": [{"name": "outfit-stylist", "prompt": "推荐穿搭"}],
    }, ["outfit-stylist"]) == ""


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
