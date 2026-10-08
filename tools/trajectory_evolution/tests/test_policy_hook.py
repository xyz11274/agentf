# test_policy_hook 回归验证：检查包内协议、边界条件与失败处理。
# 本地桩用于重现确定性场景，不代表真实 Agent 或远程模型的业务能力。
# 测试应同时验证正常路径与拒绝路径，避免错误数据被当成成功结果。

"""真实 C++ Runtime + Python Hook / 请求序列化 / 现有宿主出口回归。"""

from contextlib import nullcontext, ExitStack
import copy
import io
import json
import os
import subprocess
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import MagicMock, patch

import pytest

from test_shadow_hook import (
    BINARY,
    COMPONENT,
    ROOT,
    SDK,
    Namespace,
    load_module,
    memory_entry,
    memory_store,
    rpc_server,
)


def _batch_cli(operation, request):
    return subprocess.run(
        [str(BINARY), 'runtime', operation],
        input=json.dumps(request, ensure_ascii=False), text=True,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=10,
    )


def _batch_policy(tmp_path, count=57):
    memory = memory_store()
    memory['entries'] = [memory_entry('batch-%02d' % i) for i in range(count)]
    for i, entry in enumerate(memory['entries']):
        entry['candidate']['proposed_change'] += ' 对应独立案例序号 %d。' % i
    filename = tmp_path / 'memory.json'
    filename.write_text(json.dumps(memory, ensure_ascii=False))
    config = json.loads((COMPONENT / 'config/policy-runtime.json').read_text())
    identity = dict(agent_id='agent-a', user_id='user-a', conversation_id='conv-a', run_id='run-a')
    options = config['options']
    options.update(requested_level='L1', host_capabilities=['context.request'],
                   allowed_users=['user-a'], allowed_conversations=['conv-a'],
                   experience_template='授权经验：{{experience}}')
    request = dict(memory_path=str(filename), identity=identity,
                   shadow_options=config['shadow_options'], options=options)
    # Compile once to read the unchanged native content revisions, then approve
    # the complete snapshot rather than constructing an artificial top-k.
    provisional = _batch_cli('compile', request)
    assert provisional.returncode == 0, provisional.stdout
    policy = json.loads(provisional.stdout)
    entries = policy['snapshot']['experiences']
    assert len(entries) == count
    options['approved_experiences'] = [
        {key: entry[key] for key in ('memory_id', 'content_revision')} for entry in entries
    ]
    result = _batch_cli('compile', request)
    assert result.returncode == 0, result.stdout
    return json.loads(result.stdout), identity


def _batch_query(identity, index, **changes):
    query = dict(advice_query=dict(identity=copy.deepcopy(identity),
                 query_id='query-%d' % index, decision_id='decision-%d' % index,
                 target_scope='coordinator', task='生成视频时缺少图片素材'),
                 boundary='llm', execution_id='coordinator')
    query.update(changes)
    return query


def test_native_query_batch_keeps_full_competition_and_once_per_run(tmp_path):
    policy, identity = _batch_policy(tmp_path)
    first = _batch_query(identity, 1)
    second = _batch_query(identity, 2)
    single = _batch_cli('query', dict(policy=policy, query=first))
    assert single.returncode == 0
    result = _batch_cli('query-batch', dict(policy=policy, queries=[first, second]))
    assert result.returncode == 0, result.stdout
    decisions = json.loads(result.stdout)
    assert len(decisions) == 2
    assert decisions[0] == json.loads(single.stdout)
    assert decisions[0].get('experiences') and not decisions[1].get('experiences')
    assert not decisions[1].get('context')
    assert len(policy['snapshot']['experiences']) == 57
    # Each batch owns its runtime; changing query IDs within the first batch
    # deliberately did not reopen injection. A fresh bound runtime starts
    # with the same native decision, without retaining state across CLI calls.
    result = _batch_cli('query-batch', dict(policy=policy, queries=[first, second]))
    assert result.returncode == 0, result.stdout
    assert json.loads(result.stdout) == decisions


def test_native_query_batch_preserves_native_query_budget_and_identity(tmp_path):
    policy, identity = _batch_policy(tmp_path, 1)
    for query in [_batch_query(identity, i) for i in (1, 2, 3)]:
        if query['advice_query']['query_id'] == 'query-1':
            query['advice_query']['state'] = 'x' * 17000
        elif query['advice_query']['query_id'] == 'query-2':
            query['advice_query']['identity']['user_id'] = 'unauthorized-user'
        else:
            query['advice_query']['identity']['run_id'] = 'unbound-run'
        single = _batch_cli('query', dict(policy=policy, query=query))
        batch = _batch_cli('query-batch', dict(policy=policy, queries=[query]))
        assert batch.returncode == single.returncode
        if single.returncode:
            assert json.loads(batch.stdout) == json.loads(single.stdout)
            assert 'unauthorized-user' not in batch.stdout and 'xxxxx' not in batch.stdout
        else:
            assert json.loads(batch.stdout) == [json.loads(single.stdout)]


def test_native_query_batch_rejects_bad_shapes_and_transport_overflow(tmp_path):
    policy, identity = _batch_policy(tmp_path, 1)
    query = _batch_query(identity, 1)
    requests = [dict(policy=policy, queries={}),
                dict(policy=policy, queries=[dict(query, unknown_field='private-value')]),
                dict(policy=policy, queries=[query], unknown_field='private-value'),
                dict(policy=policy, queries=[dict(query, actor_id='x' * (2 * 1024 * 1024))])]
    for request in requests:
        result = _batch_cli('query-batch', request)
        assert result.returncode == 2
        assert json.loads(result.stdout) == {'status': 'error', 'reason': 'runtime_request_failed'}
        assert 'private-value' not in result.stdout
    empty = _batch_cli('query-batch', dict(policy=policy, queries=[]))
    assert empty.returncode == 0 and json.loads(empty.stdout) == []


@pytest.fixture
# 测试辅助函数：runtime。
# 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
def runtime(monkeypatch, tmp_path):
    from agentflow_runtime import hooks, trace
    from agentflow_runtime.hooks import engine
    from agentflow_runtime.hooks.registry import HookRegistry
    from agentflow_runtime.context import AgentContext

    module = load_module(
        COMPONENT / "integrations/agentflow/runtime_hook.py", "runtime_policy_test"
    )
    config = json.loads((COMPONENT / "config/policy-runtime.json").read_text())
    config.update(
        mode="active",
        memory_path=str(tmp_path / "memory.json"),
        executable=str(BINARY.resolve()),
        experience_template_file=str(
            COMPONENT / "config/prompts/runtime-experience.txt"
        ),
    )
    config["options"]["allowed_users"] = ["user-a", "user-b"]
    config["shadow_options"]["target_scopes"] += ["skill:file-writer"]
    memory = memory_store()
    Path(config["memory_path"]).write_text(json.dumps(memory, ensure_ascii=False))
    filename = tmp_path / "policy.json"

    # 保存当前模块的完整状态，使后台恢复或后续读取能看到一致结果。
    def save():
        filename.write_text(json.dumps(config, ensure_ascii=False))

    save()
    for key, value in {
        "AF_EVOLUTION_RUNTIME_CONFIG": str(filename),
        "AF_USER_ID": "user-a",
        "AF_CLIENT_ID": "conv-a",
        "AF_DIALOGUE_ID": "run-a",
        "AF_SESSION_ID": "shared-slot",
        "AF_AGENT_PACKAGE_DIR": str(ROOT / "examples/agent/outfit-assistant"),
        "AF_TRACE_OUTPUT_DIR": str(tmp_path),
        "AF_SESSIONLOG_DIR": str(tmp_path / "sessionlog"),
        "AF_SESSIONLOG_ENABLED": "1",
        "AF_SKIP_DOTENV": "1",
        "AF_NODE_ID": "coordinator_decide",
        "AF_SELF_TASK_ID": "coordinator",
        "AF_DATABUS_UDS_FILE": "",
    }.items():
        monkeypatch.setenv(key, value)
    for key in ("AF_EXEC_SKILL_KEY", "AF_BATCH_ID", "AF_EVOLUTION_SHADOW_CONFIG"):
        monkeypatch.delenv(key, raising=False)
    monkeypatch.setattr(trace, "current_span_id", lambda: "llm-span-a")
    namespace, records = Namespace(), []
    monkeypatch.setattr(module._shared, "_namespace", lambda _: nullcontext(namespace))
    real_record = module._shared._record
    monkeypatch.setattr(
        module._shared,
        "_record",
        lambda name, data: records.append((name, copy.deepcopy(data))),
    )
    registry = HookRegistry()
    registry.register(
        module.HOOK_NAME, module.HOOK_EVENTS, module.handle, module.HOOK_ORDER
    )
    monkeypatch.setattr(engine, "default_registry", registry)
    ctx = AgentContext(
        messages=[
            {"role": "system", "content": "保留原始规则"},
            {"role": "user", "content": "生成视频但是缺少图片素材，请继续独立网络检索"},
        ]
    )

    # 测试辅助函数：fire。
    # 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
    def fire(name, payload=None, context=None):
        return hooks.trigger(
            name,
            payload or {"round": 1},
            can_intervene=True,
            ctx=context if context is not None else ctx,
        )

    # 测试辅助函数：approve。
    # 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
    def approve():
        snapshot = module._shared._invoke(
            config,
            "compile",
            {
                "identity": module._shared._identity(config),
                "memory_path": config["memory_path"],
                "options": config["shadow_options"],
            },
        )
        config["options"]["approved_experiences"] = [
            {key: item[key] for key in ("memory_id", "content_revision")}
            for item in snapshot.get("experiences", [])
        ]
        save()

    # 测试辅助函数：receipts。
    # 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
    def receipts():
        return [data for name, data in records if name == "evolution.policy.receipt"]

    return SimpleNamespace(
        module=module,
        config=config,
        save=save,
        fire=fire,
        ctx=ctx,
        tmp=tmp_path,
        namespace=namespace,
        records=records,
        approve=approve,
        receipts=receipts,
        registry=registry,
        real_record=real_record,
    )


# 回归用例：test_disabled_is_zero_io。
# 关键断言：assert not runtime.records and not runtime.ctx.request_context；此处只验证测试构造的场景。
def test_disabled_is_zero_io(runtime, monkeypatch):
    monkeypatch.delenv("AF_EVOLUTION_RUNTIME_CONFIG")
    monkeypatch.setattr(
        runtime.module._shared, "_namespace", lambda _: pytest.fail("unexpected I/O")
    )
    for event in runtime.module.HOOK_EVENTS:
        runtime.fire(event)
    assert not runtime.records and not runtime.ctx.request_context


@pytest.mark.parametrize("mount", ["coordinator_v1", "coordinator_v2"])
# 回归用例：test_injection_exact_request_no_history_growth。
# 关键断言：assert len(ctx.request_context) == 1；此处只验证测试构造的场景。
def test_injection_exact_request_no_history_growth(runtime, mount):
    from agentflow_runtime.context import AgentContext
    from agentflow_runtime.recorder import _emit_llm_request_prepared

    runtime.approve()
    runtime.fire("agent.dialogue.start")
    original = copy.deepcopy(runtime.ctx.messages)
    for i in range(10):
        ctx = AgentContext(messages=runtime.ctx.messages)
        runtime.fire(mount + ".before_llm_call", {"round": i + 1}, ctx)
        request = {"messages": ctx.build_request_messages()}
        assert len(ctx.request_context) == 1
        assert request["messages"][0]["content"].count("<trajectory-experience") == 1
        _emit_llm_request_prepared(
            str(i),
            request,
            {},
            model="test-model",
            request_url="http://localhost/v1",
            stream=False,
            connect_timeout=1,
            request_timeout=5,
        )
        runtime.fire(mount + ".after_llm_call", {"round": i + 1}, ctx)
    assert runtime.ctx.messages == original
    assert len(runtime.receipts()) == 10
    assert all(
        item["application"]["status"] == "applied" for item in runtime.receipts()
    )
    assert not runtime.module._pending


@pytest.mark.parametrize("case", ["blocked", "removed", "missing_call", "duplicate"])
# 回归用例：test_unapplied_request_never_claims_success。
# 关键断言：assert runtime.receipts()[-1]["application"]["status"] == "failed"；此处只验证测试构造的场景。
def test_unapplied_request_never_claims_success(runtime, case):
    runtime.approve()
    runtime.fire("agent.dialogue.start")
    runtime.fire("coordinator_v1.before_llm_call")
    request = {"messages": runtime.ctx.build_request_messages()}
    if case == "removed":
        request["messages"][0]["content"] = "another hook replaced context"
    if case == "duplicate":
        request["messages"][0]["content"] += runtime.ctx.request_context[0]
    if case != "missing_call":
        runtime.fire(
            "llm.request.prepared",
            {
                "request": request,
                "call_id": "c1",
                "transport_allowed": case != "blocked",
            },
        )
    runtime.fire("coordinator_v1.after_llm_call")
    assert runtime.receipts()[-1]["application"]["status"] == "failed"
    assert not runtime.module._pending


# 回归用例：test_same_name_dispatch_only_one_rejected_and_indices_preserved。
# 重点核验字段或诊断：tool_call_id、skip_request_indices；异常路径不能被当成成功。
def test_same_name_dispatch_only_one_rejected_and_indices_preserved(runtime):
    from agentflow_runtime.coordinator_decide import handle_run_skills_call

    runtime.config["options"]["action_rules"] = [
        {
            "rule_id": "needs-image",
            "target_scope": "coordinator",
            "operation": "video",
            "required_arguments": ["load_skills"],
            "reason": "需要显式加载技能",
        }
    ]
    runtime.save()
    runtime.fire("agent.dialogue.start")
    skills = [
        {"name": "video", "prompt": "生成视频"},
        {"name": "video", "prompt": "使用提供的图片", "load_skills": ["video"]},
        {"name": "search", "prompt": "独立查询"},
    ]
    payload = runtime.fire(
        "coordinator.before_skill_dispatch",
        {
            "round": 1,
            "tool_call_id": "batch-call",
            "skills": skills,
            "skip_request_indices": {},
        },
    )
    bus = MagicMock()
    bus.put_global.return_value = True
    bus.put.return_value = True
    result = json.loads(
        handle_run_skills_call(
            {"skills": skills},
            bus,
            "slot",
            "run-a",
            skip_request_indices=payload.get("skip_request_indices"),
        )
    )
    assert result["status"] == "partially_dispatched"
    assert [item["request_index"] for item in result["dispatches"]] == [2, 3]
    assert result["rejected"][0]["stage"] == "policy_gate"
    runtime.fire(
        "coordinator.after_skill_dispatch", {**result, "tool_call_id": "batch-call"}
    )
    assert runtime.receipts()[-1]["action_ids"] == ["batch-call:1"]
    assert runtime.receipts()[-1]["application"]["status"] == "applied"


# 测试辅助函数：_rule。
# 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
def _rule(scope="coordinator"):
    return {
        "rule_id": "bounded:" + scope,
        "target_scope": scope,
        "max_iterations": 4,
        "max_no_progress": 1,
        "no_progress_decision": "replan",
        "max_replans": 1,
        "replan_context": "停止重复请求，请寻找其他可行方案。",
        "reason": "达到已授权的无进展边界",
    }


# 回归用例：test_loop_replan_requires_actual_request_and_stops_only_current_execution。
# 重点核验字段或诊断：tool_call_id；异常路径不能被当成成功。
def test_loop_replan_requires_actual_request_and_stops_only_current_execution(
    runtime, monkeypatch
):
    from agentflow_runtime.loop_control import decide_loop
    from agentflow_runtime.context import AgentContext

    runtime.config["options"]["loop_rules"] = [_rule(), _rule("skill:file-writer")]
    runtime.save()
    runtime.fire("agent.dialogue.start")
    runtime.ctx.messages.append(
        {"role": "tool", "tool_call_id": "tool-a", "content": "no progress"}
    )
    decisions = []
    for i in range(1, 4):
        payload, ctx = decide_loop(
            runtime.ctx.messages,
            target_scope="coordinator",
            execution_id="coordinator",
            iteration=i,
        )
        decisions.append(payload["decision"])
        if i == 2:
            assert not runtime.receipts(), (
                "replan queued must not be counted as applied"
            )
            llm_ctx = AgentContext(
                messages=runtime.ctx.messages, request_context=ctx.request_context
            )
            runtime.fire("coordinator_v1.before_llm_call", {"round": i}, llm_ctx)
            runtime.fire(
                "llm.request.prepared",
                {
                    "call_id": "replan-call",
                    "transport_allowed": True,
                    "request": {"messages": llm_ctx.build_request_messages()},
                },
            )
    assert decisions == ["continue", "replan", "stop"]
    assert [item["application"]["status"] for item in runtime.receipts()] == [
        "applied",
        "applied",
    ]
    monkeypatch.setenv("AF_EXEC_SKILL_KEY", "dispatch:another")
    monkeypatch.setenv("AF_SKILL_NAME", "file-writer")
    decision, _ = decide_loop(
        runtime.ctx.messages,
        target_scope="skill:file-writer",
        execution_id="dispatch:another",
        iteration=1,
    )
    assert decision["decision"] == "continue"


# 回归用例：test_later_hook_rejection_does_not_persist_an_unapplied_stop。
# 关键断言：assert payload["decision"] == "continue"；此处只验证测试构造的场景。
def test_later_hook_rejection_does_not_persist_an_unapplied_stop(runtime):
    from agentflow_runtime.loop_control import decide_loop

    runtime.config["options"]["loop_rules"] = [_rule()]
    runtime.config["options"]["loop_rules"][0]["max_iterations"] = 1
    runtime.save()
    runtime.fire("agent.dialogue.start")
    runtime.registry.register(
        "host-declines",
        ["loop.before_decision"],
        lambda event, ctx: {"decision": "continue", "reason": "host_declined"},
        300,
    )
    for i in [1, 2]:
        payload, _ = decide_loop(
            runtime.ctx.messages,
            target_scope="coordinator",
            execution_id="coordinator",
            iteration=i,
        )
        assert payload["decision"] == "continue"
    progress = [
        json.loads(value)
        for key, value in runtime.namespace.data.items()
        if "/loop/" in key
    ]
    assert progress[0]["decision"] == "continue"
    assert runtime.receipts()[-1]["application"]["status"] == "failed"
    assert not runtime.module._loop_updates


# 回归用例：test_no_lazy_compile_no_midrun_version_change。
# 关键断言：assert not runtime.namespace.data；此处只验证测试构造的场景。
def test_no_lazy_compile_no_midrun_version_change(runtime):
    runtime.fire("coordinator_v1.before_llm_call")
    assert not runtime.namespace.data
    runtime.approve()
    runtime.fire("agent.dialogue.start")
    snapshot = copy.deepcopy(runtime.namespace.data)
    runtime.config["options"]["approved_experiences"] = []
    runtime.save()
    runtime.fire("agent.dialogue.start")
    assert runtime.namespace.data == snapshot
    runtime.fire("coordinator_v1.before_llm_call")
    assert runtime.ctx.request_context, "new config must wait until next run"
    runtime.fire("agent.dialogue.end")
    assert not runtime.namespace.data and not runtime.module._pending


# 回归用例：test_secret_in_query_is_redacted。
# 关键断言：assert secret not in json.dumps(runtime.records, ensure_ascii=False)；此处只验证测试构造的场景。
def test_secret_in_query_is_redacted(runtime, monkeypatch):
    secret = "synthetic-private-runtime-secret-abcdef"
    monkeypatch.setenv("CUSTOM_PROVIDER_TOKEN", secret)
    runtime.approve()
    runtime.fire("agent.dialogue.start")
    runtime.ctx.messages.append(
        {"role": "user", "content": "生成视频图片素材 " + secret}
    )
    runtime.fire("coordinator_v1.before_llm_call")
    assert secret not in json.dumps(runtime.records, ensure_ascii=False)
    assert secret not in json.dumps(
        runtime.ctx.build_request_messages(), ensure_ascii=False
    )


# 回归用例：test_skill_real_exit_keeps_skill_end_and_no_phantom_llm。
# 重点核验字段或诊断：load_skills_from_list、load_hooks_from_dir、resolve_llm_config；异常路径不能被当成成功。
def test_skill_real_exit_keeps_skill_end_and_no_phantom_llm(runtime, monkeypatch):
    from agentflow_runtime import skill_executor as executor
    from agentflow_runtime.context import AgentContext
    from agentflow_runtime.hooks import trigger

    runtime.config["options"]["loop_rules"] = [_rule("skill:file-writer")]
    runtime.config["options"]["loop_rules"][0]["max_iterations"] = 1
    runtime.save()
    runtime.fire("agent.dialogue.start")
    monkeypatch.setenv("AF_EXEC_SKILL_KEY", "dispatch:worker")
    monkeypatch.setenv("AF_NODE_ID", "skill_executor")
    monkeypatch.setenv("AF_INPUT", "读取数据文件")
    bus = MagicMock()
    bus.is_connected.return_value = True
    bus.get.return_value = None
    bus.get_global.side_effect = lambda key: (
        json.dumps({"skill_name": "file-writer", "prompt": "读取数据文件"})
        if key.startswith("dispatch:")
        else None
    )
    registry = MagicMock()
    registry.get_tools.return_value = []
    registry.execute.return_value = '{"stdout":"ok","exit_code":0}'
    response = {
        "choices": [
            {
                "message": {
                    "role": "assistant",
                    "content": "",
                    "tool_calls": [
                        {
                            "id": "read-a",
                            "type": "function",
                            "function": {
                                "name": "read_file",
                                "arguments": '{"path":"a"}',
                            },
                        }
                    ],
                },
                "finish_reason": "tool_calls",
            }
        ],
        "usage": {"total_tokens": 1},
    }
    seen = []
    runtime.registry.register(
        "record-end",
        ["skill_executor.after_skill_complete"],
        lambda event, ctx: seen.append(event.payload),
        100,
    )
    recorder = MagicMock()
    recorder.record_effective_response.side_effect = lambda _, raw: raw
    with ExitStack() as stack:
        for name, value in {
            "DataBusClient": MagicMock(return_value=bus),
            "_LocalToolRegistry": MagicMock(return_value=registry),
            "load_skills_from_list": MagicMock(return_value={}),
            "get_recorder": MagicMock(return_value=recorder),
            "load_hooks_from_dir": MagicMock(),
            "hook_trigger": trigger,
            "compress_messages": lambda messages, *a, **kw: (messages, "skip"),
            "resolve_llm_config": lambda *a, **kw: {
                "model": "test",
                "base_url": "http://localhost/v1",
                "api_key": "unit-key",
            },
        }.items():
            stack.enter_context(patch.object(executor, name, value))
        cfg = stack.enter_context(patch.object(executor, "AgentConfig"))
        cfg.skill_max_iterations.return_value = 5
        cfg.node_execution_timeout.return_value = 3600
        cfg.skill_max_completion_tokens.return_value = 1000
        cfg.skill_llm_recovery_attempts.return_value = 1
        llm = stack.enter_context(
            patch.object(executor, "call_llm", return_value=response)
        )
        stack.enter_context(patch("sys.stdout", io.StringIO()))
        executor.execute()
    assert llm.call_count == 1
    assert len(seen) == 1 and seen[0]["status"] == "policy_stopped"
    terminal = [
        json.loads(args.args[1])
        for args in bus.put.call_args_list
        if args.args[0] == "skill_result:skill-runner"
    ]
    assert (
        terminal[-1]["status"] == "policy_stopped" and terminal[-1]["rounds_used"] == 1
    )
    assert not runtime.module._pending


# 回归用例：test_request_context_respects_later_hooks。
# 关键断言：assert runtime.ctx.build_request_messages()[0]["content"].startswith("later system")；此处只验证测试构造的场景。
def test_request_context_respects_later_hooks(runtime):
    runtime.approve()
    runtime.fire("agent.dialogue.start")
    runtime.registry.register(
        "later-hook",
        ["coordinator_v1.before_llm_call"],
        lambda event, ctx: ctx.messages[0].update(content="later system"),
        300,
    )
    runtime.fire("coordinator_v1.before_llm_call")
    assert runtime.ctx.build_request_messages()[0]["content"].startswith("later system")
    assert runtime.ctx.messages[0]["content"] == "later system"


# 回归用例：test_hot_disable_cannot_erase_an_already_applied_request。
# 关键断言：assert runtime.receipts()[-1]["application"]["status"] == "applied"；此处只验证测试构造的场景。
def test_hot_disable_cannot_erase_an_already_applied_request(runtime):
    runtime.approve()
    runtime.fire("agent.dialogue.start")
    runtime.fire("coordinator_v1.before_llm_call")
    runtime.config["mode"] = "disabled"
    runtime.save()
    runtime.fire(
        "llm.request.prepared",
        {
            "request": {"messages": runtime.ctx.build_request_messages()},
            "call_id": "sent-before-disable",
            "transport_allowed": True,
        },
    )
    assert runtime.receipts()[-1]["application"]["status"] == "applied"
    assert not runtime.module._pending


# 回归用例：test_hot_disable_flushes_a_replan_that_never_reached_llm。
# 重点核验字段或诊断：tool_call_id；异常路径不能被当成成功。
def test_hot_disable_flushes_a_replan_that_never_reached_llm(runtime):
    from agentflow_runtime.loop_control import decide_loop

    runtime.config["options"]["loop_rules"] = [_rule()]
    runtime.save()
    runtime.fire("agent.dialogue.start")
    runtime.ctx.messages.append(
        {"role": "tool", "content": "unchanged", "tool_call_id": "t1"}
    )
    for iteration in [1, 2]:
        decide_loop(
            runtime.ctx.messages,
            target_scope="coordinator",
            execution_id="coordinator",
            iteration=iteration,
        )
    assert runtime.module._replans
    runtime.config["mode"] = "disabled"
    runtime.save()
    runtime.fire("agent.dialogue.end")
    assert not runtime.module._replans and not runtime.module._loop_updates
    assert runtime.receipts()[-1]["application"]["status"] == "failed"


# 回归用例：test_receipt_failure_does_not_block_llm_or_lose_query_identity。
# 关键断言：assert receipt["application"]["status"] == "failed"；此处只验证测试构造的场景。
def test_receipt_failure_does_not_block_llm_or_lose_query_identity(
    runtime, monkeypatch
):
    runtime.approve()
    runtime.fire("agent.dialogue.start")
    runtime.fire("coordinator_v1.before_llm_call")
    monkeypatch.setattr(
        runtime.module,
        "_invoke",
        lambda *a, **kw: (_ for _ in ()).throw(TimeoutError()),
    )
    runtime.fire(
        "llm.request.prepared",
        {
            "request": {"messages": runtime.ctx.build_request_messages()},
            "call_id": "c1",
            "transport_allowed": True,
        },
    )
    receipt = runtime.receipts()[-1]
    assert receipt["application"]["status"] == "failed"
    assert receipt["application"]["query_id"].startswith("runtime-query:")
    assert not runtime.module._pending


@pytest.mark.parametrize("version", ["v1", "v2"])
# 回归用例：test_coordinator_actual_loop_exit_and_v2_action_isolation。
# 重点核验字段或诊断：load_hooks_from_dir、parse_agent_md、resolve_llm_config；异常路径不能被当成成功。
def test_coordinator_actual_loop_exit_and_v2_action_isolation(
    runtime, monkeypatch, version
):
    from agentflow_runtime import (
        coordinator_decide,
        coordinator_agent_loop,
        skill_executor,
    )

    executor = coordinator_decide if version == "v1" else coordinator_agent_loop
    runtime.config["options"]["loop_rules"] = [_rule()]
    runtime.config["options"]["loop_rules"][0]["max_iterations"] = 1
    runtime.config["options"]["action_rules"] = [
        {
            "rule_id": "path-required",
            "target_scope": "coordinator",
            "operation": "read_file",
            "required_arguments": ["path"],
            "reason": "缺少文件路径",
        }
    ]
    runtime.save()
    monkeypatch.setenv("AF_INPUT", "读取数据并汇总")
    monkeypatch.setenv("AF_NODE_ID", "coordinator_decide")
    for key in ("AF_COMPLETED_SKILL_NAME", "AF_COMPLETED_BATCH_KEY", "AF_SKILL_RESULT"):
        monkeypatch.delenv(key, raising=False)
    # v2 会同步 skill_executor 路径，测试结束还原到测试前的值。
    monkeypatch.setattr(
        skill_executor, "AGENT_PACKAGE_DIR", skill_executor.AGENT_PACKAGE_DIR
    )
    monkeypatch.setattr(skill_executor, "AGENT_MD_PATH", skill_executor.AGENT_MD_PATH)
    bus = MagicMock()
    bus.get.return_value = None
    bus.get_global.return_value = None
    bus.is_connected.return_value = False
    recorder = MagicMock()
    recorder.is_connected = False
    recorder.record_effective_response.side_effect = lambda _, raw: raw
    registry = MagicMock()
    registry.get_tools.return_value = []
    registry.execute.side_effect = lambda _, args: (
        '{"exit_code":0,"stdout":"ok"}'
        if args["path"] == "ok"
        else '{"error":"expected branch failure"}'
    )
    calls = (
        [
            {
                "id": "invalid-dispatch",
                "type": "function",
                "function": {"name": "run_skills", "arguments": '{"skills":[]}'},
            }
        ]
        if version == "v1"
        else [
            {
                "id": f"read-{i}",
                "type": "function",
                "function": {
                    "name": "read_file",
                    "arguments": json.dumps({"path": path}),
                },
            }
            for i, path in enumerate(["", "ok", "failure"], 1)
        ]
    )
    raw = {
        "choices": [
            {
                "message": {"role": "assistant", "content": "", "tool_calls": calls},
                "finish_reason": "tool_calls",
            }
        ],
        "usage": {"total_tokens": 1},
    }
    ends, tool_results = [], []
    runtime.registry.register(
        "actual-ends",
        ["agent.dialogue.end"],
        lambda event, ctx: ends.append(event.payload),
        100,
    )
    runtime.registry.register(
        "actual-tools",
        ["coordinator_v2.after_tool_call"],
        lambda event, ctx: tool_results.extend(event.payload["tool_results"]),
        100,
    )
    with ExitStack() as stack:
        for name, value in {
            "DataBusClient": MagicMock(return_value=bus),
            "get_recorder": lambda: recorder,
            "load_hooks_from_dir": MagicMock(),
            "_load_dotenv": MagicMock(),
            "parse_agent_md": lambda *_: ({}, "test system"),
            "compress_messages": lambda messages, *a, **kw: (messages, "skip"),
            "resolve_llm_config": lambda *a, **kw: {
                "model": "test",
                "api_key": "unit-key",
                "base_url": "http://localhost/v1",
            },
        }.items():
            stack.enter_context(patch.object(executor, name, value))
        stack.enter_context(
            patch.object(
                executor.AgentConfig, "coordinator_max_iterations", return_value=5
            )
        )
        stack.enter_context(
            patch.object(
                executor.AgentConfig, "node_execution_timeout", return_value=3600
            )
        )
        if version == "v2":
            stack.enter_context(
                patch.object(executor, "_LocalToolRegistry", return_value=registry)
            )
            stack.enter_context(patch.object(executor, "_inject_venv_env"))
        else:
            stack.enter_context(patch.object(executor, "_init_skills"))
        llm = stack.enter_context(patch.object(executor, "call_llm", return_value=raw))
        output = stack.enter_context(patch("sys.stdout", io.StringIO()))
        executor.execute()
    assert llm.call_count == 1
    assert len(ends) == 1 and ends[0]["status"] == "policy_stopped"
    assert json.loads(output.getvalue().splitlines()[-1])["status"] == "policy_stopped"
    if version == "v2":
        assert registry.execute.call_count == 2
        assert [(item["tool_call_id"], item["status"]) for item in tool_results] == [
            ("read-1", "skipped"),
            ("read-2", "completed"),
            ("read-3", "error"),
        ]
        assert any(
            item.get("action_ids") == ["read-1"]
            and item["application"]["status"] == "applied"
            for item in runtime.receipts()
        )
    assert not runtime.module._pending and not runtime.module._loop_updates


# 回归用例：test_sdk_two_users_three_conversations_and_fresh_process。
# 重点核验字段或诊断：before_llm_call；异常路径不能被当成成功。
def test_sdk_two_users_three_conversations_and_fresh_process(
    runtime, monkeypatch, rpc_server
):
    import sys
    import subprocess

    monkeypatch.syspath_prepend(str(SDK))
    monkeypatch.setenv("AF_DATABUS_UDS_FILE", str(rpc_server.server_address))
    identities = [("user-a", "conv-a"), ("user-a", "conv-b"), ("user-b", "conv-c")]
    # 真实 SDK / 子进程重新加载 Hook，每个 Run 冻结一次，跨 Run 不复用身份。
    program = """
import importlib.util, json, os
from types import SimpleNamespace
from agentflow_runtime.context import AgentContext
spec = importlib.util.spec_from_file_location('runtime_hook', os.environ['POLICY_TEST_HOOK'])
hook = importlib.util.module_from_spec(spec); spec.loader.exec_module(hook)
records = []
hook._shared._record = lambda n, p: records.append((n, p))
ctx = AgentContext(messages=[{'role':'user','content':'生成视频缺少图片素材'}])
hook.handle(SimpleNamespace(name=os.environ['POLICY_TEST_EVENT'], payload={'round':1}), ctx)
print(json.dumps({'records': records, 'context': ctx.request_context}, ensure_ascii=False))
"""
    for user, conv in identities:
        monkeypatch.setenv("AF_USER_ID", user)
        monkeypatch.setenv("AF_CLIENT_ID", conv)
        runtime.approve()
        for turn in range(2):
            monkeypatch.setenv("AF_DIALOGUE_ID", f"run-{user}-{conv}-{turn}")
            for event in [
                "agent.dialogue.start",
                "coordinator_v1.before_llm_call",
                "agent.dialogue.end",
            ]:
                env = dict(
                    os.environ,
                    POLICY_TEST_HOOK=str(
                        COMPONENT / "integrations/agentflow/runtime_hook.py"
                    ),
                    POLICY_TEST_EVENT=event,
                    PYTHONPATH=str(SDK)
                    + os.pathsep
                    + str(ROOT / "packages/agentflow-runtime/src"),
                )
                completed = subprocess.run(
                    [sys.executable, "-c", program],
                    env=env,
                    capture_output=True,
                    check=True,
                )
                output = json.loads(completed.stdout)
                assert not any(
                    name == "evolution.policy.error" for name, _ in output["records"]
                )
                if event.endswith("before_llm_call"):
                    assert output["context"]
                    record = next(
                        data
                        for name, data in output["records"]
                        if name == "evolution.policy.query"
                    )
                    assert (
                        record["identity"]["user_id"] == user
                        and record["identity"]["conversation_id"] == conv
                    )
            assert rpc_server.idle.wait(2), "SDK connection leaked"
    assert not rpc_server.data


# 回归用例：test_http_body_receipt_and_sessionlog。
# 重点核验字段或诊断：source_event_ids；异常路径不能被当成成功。
def test_http_body_receipt_and_sessionlog(runtime, monkeypatch):
    import threading
    from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
    from agentflow_runtime.context import AgentContext
    from agentflow_runtime.recorder import TraceRecorder

    received = []

    class Handler(BaseHTTPRequestHandler):
        # 测试辅助函数：do_POST。
        # 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
        def do_POST(self):
            received.append(
                json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            )
            body = json.dumps(
                {
                    "choices": [
                        {
                            "message": {
                                "role": "assistant",
                                "content": "请补充图片素材，独立检索继续。",
                            },
                            "finish_reason": "stop",
                        }
                    ],
                    "usage": {"total_tokens": 3},
                }
            ).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        # 测试辅助函数：log_message。
        # 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
        def log_message(self, *args):
            pass

    sessionlog = load_module(
        ROOT / "docs/sessionlog/sessionlog_hook.py", "runtime_sessionlog"
    )
    monkeypatch.setattr(sessionlog, "_get_databus", lambda: None)
    monkeypatch.setattr(sessionlog, "_current_span_id", lambda: "llm-span-a")
    runtime.registry.register(
        sessionlog.HOOK_NAME, sessionlog.HOOK_EVENTS, sessionlog.handle, 100
    )
    monkeypatch.setattr(runtime.module._shared, "_record", runtime.real_record)
    monkeypatch.setattr(TraceRecorder, "_instance", None)
    recorder = TraceRecorder()
    runtime.approve()
    runtime.fire("agent.dialogue.start", {"input": "生成视频缺少图片素材"})
    runtime.fire(
        "coordinator.trigger", {"trigger_type": "first", "activation_index": 1}
    )
    with ThreadingHTTPServer(("127.0.0.1", 0), Handler) as server:
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            for i in range(3):
                ctx = AgentContext(messages=runtime.ctx.messages)
                runtime.fire(
                    "coordinator_v1.before_llm_call",
                    {"round": i + 1, "model": "http-test"},
                    ctx,
                )
                raw = recorder.trace_call(
                    ctx.build_request_messages(),
                    model="http-test",
                    base_url=f"http://127.0.0.1:{server.server_port}/v1",
                    api_key="synthetic-local-http-key",
                    source="coordinator",
                    round_i=i + 1,
                    timeout=10,
                )
                assert not raw.get("_llm_error")
                runtime.fire("coordinator_v1.after_llm_call", {"round": i + 1}, ctx)
                runtime.fire(
                    "coordinator.llm_result",
                    {
                        "round": i + 1,
                        "call_id": raw["_agentflow_call_id"],
                        "response": raw,
                    },
                    ctx,
                )
        finally:
            server.shutdown()
            thread.join(timeout=2)
    runtime.fire("agent.dialogue.end", {"status": "completed", "response_text": "完成"})
    assert len(received) == 3
    assert all(
        item["messages"][0]["content"].count("<trajectory-experience") == 1
        for item in received
    )
    files = list(runtime.tmp.rglob("run-a.jsonl"))
    assert len(files) == 1
    rows = [json.loads(line) for line in files[0].read_text().splitlines()]
    receipts = [row for row in rows if row["type"] == "evolution.policy.receipt"]
    requests = {
        row["data"]["call_id"]: row
        for row in rows
        if row["type"] == "llm.transport.request"
    }
    assert len(receipts) == 3
    for receipt in receipts:
        assert receipt["data"]["application"]["status"] == "applied"
        assert receipt["source_event_ids"] == [
            requests[receipt["data"]["call_id"]]["event_id"]
        ]
    assert "synthetic-local-http-key" not in files[0].read_text()


@pytest.mark.skipif(
    os.environ.get("AF_EVOLUTION_LIVE_TEST") != "1",
    reason="explicit live API opt-in required",
)
# 回归用例：test_live_model_injection。
# 关键断言：assert api_key, "configure the key in the environment, never in test source"；此处只验证测试构造的场景。
def test_live_model_injection(runtime, monkeypatch):
    from agentflow_runtime.context import AgentContext
    from agentflow_runtime.recorder import TraceRecorder

    api_key = os.environ.get("OPENAI_API_KEY", "")
    assert api_key, "configure the key in the environment, never in test source"
    monkeypatch.setattr(TraceRecorder, "_instance", None)
    runtime.approve()
    runtime.fire("agent.dialogue.start")
    recorder = TraceRecorder()
    responses = []
    for i in range(3):
        ctx = AgentContext(messages=runtime.ctx.messages)
        runtime.fire("coordinator_v1.before_llm_call", {"round": i + 1}, ctx)
        raw = recorder.trace_call(
            ctx.build_request_messages(),
            model=os.environ.get("EVOLUTION_TEST_MODEL", "deepseek-v4-flash"),
            base_url=os.environ["OPENAI_BASE_URL"],
            api_key=api_key,
            max_completion_tokens=1024,
            timeout=int(os.environ.get("EVOLUTION_TEST_TIMEOUT", "1800")),
            connect_timeout=15,
            source="coordinator",
            round_i=i + 1,
            extra_body={"thinking": {"type": "disabled"}},
        )
        assert not raw.get("_llm_error"), (
            "live transport/model failed; inspect redacted trace"
        )
        content = raw.get("choices", [{}])[0].get("message", {}).get("content", "")
        assert content, "live model returned no visible answer"
        responses.append(
            {
                "call_id": raw["_agentflow_call_id"],
                "content": content,
                "usage": raw.get("usage", {}),
            }
        )
        runtime.ctx.messages.append({"role": "assistant", "content": content})
        runtime.ctx.messages.append(
            {
                "role": "user",
                "content": "图片素材仍未提供，请只说明视频生成的前置条件和独立检索下一步，100字以内。",
            }
        )
        runtime.fire("coordinator_v1.after_llm_call", {"round": i + 1}, ctx)
    assert len(runtime.receipts()) == 3 and all(
        item["application"]["status"] == "applied" for item in runtime.receipts()
    )
    result = {
        "model": os.environ.get("EVOLUTION_TEST_MODEL", "deepseek-v4-flash"),
        "thinking": "disabled",
        "responses": responses,
        "receipts": runtime.receipts(),
    }
    from agentflow_runtime.security import redact_secrets

    result, _ = redact_secrets(result)
    encoded = json.dumps(result, ensure_ascii=False, indent=2)
    assert api_key not in encoded
    (runtime.tmp / "live-acceptance.json").write_text(encoded, encoding="utf-8")
